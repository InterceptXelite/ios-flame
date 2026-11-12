/* Touch firmware blob for the stage-6 shim test (proprietary, never committed:
 * build.sh passes the path of research/touchfw/<file> as TOUCH_FW_PATH). */
	.section .rodata.touchfw, "a"
	.balign 16
	.global touch_fw_start, touch_fw_end
touch_fw_start:
#ifdef TOUCH_FW_PATH
	.incbin TOUCH_FW_PATH
#endif
touch_fw_end:
