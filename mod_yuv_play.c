#include <switch.h>

static struct
{
    char *str;
} globals;

void safe_free_globals()
{
    if (globals.str)
    {
        free(globals.str);
    }
}

/*
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_play_yuv_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_play_yuv_runtime);
*/

SWITCH_MODULE_LOAD_FUNCTION(mod_play_yuv_load);
SWITCH_MODULE_DEFINITION(mod_play_yuv, mod_play_yuv_load, NULL, NULL);

static switch_status_t load_config(void)
{
    const char *cf = "rtpplay.conf";
    switch_xml_t cfg, xml, settings, param;
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL)))
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
        return SWITCH_STATUS_TERM;
    }

    if ((settings = switch_xml_child(cfg, "settings")))
    {
        for (param = switch_xml_child(settings, "param"); param; param = param->next)
        {
            char *var = (char *)switch_xml_attr_soft(param, "name");
            char *val = (char *)switch_xml_attr_soft(param, "value");

            if (!strcasecmp(var, "str"))
            {
                globals.str = strdup(val);
            }
        }
    }

    switch_xml_free(xml);

    return status;
}

SWITCH_STANDARD_APP(yuv_play_function)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_frame_t vid_frame = {0};
    int fd = -1;
    switch_codec_t *codec = NULL;
    unsigned char *vid_buffer;
    // switch_timer_t timer = { 0 };
    switch_dtmf_t dtmf = {0};
    switch_frame_t *read_frame;
    uint32_t width = 0, height = 0, fps = 0, size;
    switch_image_t *img = NULL;
    switch_byte_t *yuv = NULL;
    int argc;
    char *argv[5] = {0};
    char *mydata = switch_core_session_strdup(session, data);
    uint32_t loops = 0;
    // switch_bool_t b_play_over = SWITCH_FALSE;

    switch_channel_answer(channel);

    switch_core_session_request_video_refresh(session);

    switch_channel_audio_sync(channel);
    switch_core_session_raw_read(session);

    argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

    if (argc == 0)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No file present\n");
        goto done;
    }

    if (argc > 1)
        width = atoi(argv[1]);
    if (argc > 2)
        height = atoi(argv[2]);
    if (argc > 3)
        fps = atoi(argv[3]);

    // switch_channel_set_flag(channel, CF_VIDEO_DECODED_READ);

    while (switch_channel_ready(channel) && !switch_channel_test_flag(channel, CF_VIDEO))
    {
        if ((++loops % 100) == 0)
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Waiting for video......\n");
        switch_ivr_sleep(session, 20, SWITCH_TRUE, NULL);

        continue;
    }

    width = width ? width : 352;
    height = height ? height : 288;
    size = width * height * 3 / 2;
    fps = fps ? fps : 20;

    img = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420, width, height, 0);

    if (!img)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory Error\n");
        goto end;
    }

    yuv = img->planes[SWITCH_PLANE_PACKED];

    switch_channel_set_flag(channel, CF_VIDEO_WRITING);
    //SWITCH_RTP_MAX_BUF_LEN
    vid_buffer = switch_core_session_alloc(session, SWITCH_RTP_MAX_BUF_LEN);

    switch_channel_set_variable(channel, SWITCH_PLAYBACK_TERMINATOR_USED, "");

    if ((fd = open(argv[0], O_RDONLY | O_BINARY)) < 0)
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Error opening file %s\n", (char *)data);
        switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Got error while opening file");
        goto end;
    }

    codec = switch_core_session_get_video_write_codec(session);

    vid_frame.codec = codec;
    vid_frame.packet = vid_buffer;
    vid_frame.data = ((uint8_t *)vid_buffer) + 12;
    vid_frame.buflen = SWITCH_RTP_MAX_BUF_LEN - 12;
    switch_set_flag((&vid_frame), SFF_RAW_RTP);
    // switch_set_flag((&vid_frame), SFF_PROXY_PACKET);

    switch_core_session_request_video_refresh(session);
    switch_core_media_gen_key_frame(session);

    while (switch_channel_ready(channel) /* && b_play_over == SWITCH_FALSE */)
    {
        if (read(fd, yuv, size) != (int)size)
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Error reading file\n");
            switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Got error reading file");
            goto end;
        }

        switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

        if (switch_channel_test_flag(channel, CF_BREAK))
        {
            switch_channel_clear_flag(channel, CF_BREAK);
            break;
        }

        switch_ivr_parse_all_events(session);

        //check for dtmf interrupts
        if (switch_channel_has_dtmf(channel))
        {
            const char *terminators = switch_channel_get_variable(channel, SWITCH_PLAYBACK_TERMINATORS_VARIABLE);
            switch_channel_dequeue_dtmf(channel, &dtmf);

            if (terminators && !strcasecmp(terminators, "none"))
            {
                terminators = NULL;
            }

            if (terminators && strchr(terminators, dtmf.digit))
            {
                char sbuf[2] = {dtmf.digit, '\0'};

                switch_channel_set_variable(channel, SWITCH_PLAYBACK_TERMINATOR_USED, sbuf);
                break;
            }
        }

        if (read_frame)
        {
            memset(read_frame->data, 0, read_frame->datalen);
            switch_core_session_write_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);
        }

        vid_frame.img = img;
        switch_core_session_write_video_frame(session, &vid_frame, SWITCH_IO_FLAG_NONE, 0);

        switch_yield(1000 / fps * 1000);
    }

    switch_core_thread_session_end(session);
    switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "OK");

end:

    if (fd > -1)
    {
        close(fd);
    }

    switch_img_free(&img);

done:

    switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
    switch_core_session_video_reset(session);
    switch_channel_clear_flag(channel, CF_VIDEO_WRITING);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_play_yuv_load)
{
    switch_application_interface_t *app_interface = NULL;

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    memset(&globals, 0, sizeof(globals));
    load_config();

    /* indicate that the module should continue to be loaded */

    SWITCH_ADD_APP(app_interface, "yuv_play", "play a yuv file", "play a yuv file", yuv_play_function, "<file> [width] [height]", SAF_NONE);

    return SWITCH_STATUS_SUCCESS;
}

// Called when the system shuts down
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rtpplay_shutdown)
{
    safe_free_globals();
    return SWITCH_STATUS_SUCCESS;
}

/*
  If it exists, this is called in it's own thread when the module-load completes
  If it returns anything but SWITCH_STATUS_TERM it will be called again automaticly
SWITCH_MODULE_RUNTIME_FUNCTION(mod_rtpplay_runtime);
{
	while(looping)
	{
		switch_yield(1000);
	}
	return SWITCH_STATUS_TERM;
}
*/

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet expandtab:
 */
