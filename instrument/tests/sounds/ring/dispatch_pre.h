/* around the extracted main-loop dispatch block only: each name it calls goes through a recorder (harness.c), which
 * then calls the real function. The block's own text is untouched. */
#define ann_frame hs_ann_frame
#define log_frame hs_log_frame
#define remote_frame hs_remote_frame
#define app_sysex hs_app_sysex
#define ota_service hs_ota_service
#define usb_cmd hs_usb_cmd
#define app_panic hs_app_panic
#define ota_session hs_ota_session
#define enter_uboot hs_enter_uboot
#define bu_step hs_bu_step
#define fm1_ms now_ms
