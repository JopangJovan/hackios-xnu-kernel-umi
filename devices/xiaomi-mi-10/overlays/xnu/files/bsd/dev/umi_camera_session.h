/* The vnode mutex serializes capture, camera switch and close. */
int
umi_camera_ios_capture(void)
{
    if(umi_cam_leaving)return ENXIO;
    if(!uc.active) {
        uc.front=umi_cam_sensor==2;
        umi_cam_session=true;
        int error=uprobe(8);
        if(error){ucleanup();uc.front=false;}
        return error;
    }
    if(!umi_cam_session)return EBUSY;
    if(uc.front!=(umi_cam_sensor==2)) {ucleanup();uc.front=false;return EAGAIN;}
    int error=uisp_capture_reset();
    if(error){ucleanup();uc.front=false;return error;}
    umi_cam_frame_focus=HAKIOS_CAMERA_NO_FOCUS;
    if(!uc.front) {
        int focus_error=ufocus_apply(umi_cam_focus);
        if(!focus_error)umi_cam_frame_focus=umi_cam_focus;
        else hakios_tl("UMI focus: retained position failed errno=%d\n",focus_error);
    }
    error=uc.front?umi_camera_front_frame(isp_ife,isp_csid,usensor_stream):
        umi_camera_sensor_frame(isp_ife,isp_csid,usensor_stream);
    if(error){ucleanup();uc.front=false;}
    return error;
}

void
umi_camera_ios_release(void)
{
    ucleanup();
    uc.front=false;
}
