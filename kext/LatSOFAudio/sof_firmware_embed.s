.section __DATA,__sof_fw
.globl _sof_fw_data
.globl _sof_fw_size
_sof_fw_data:
.incbin "LatSOFAudio/Firmware/sof-cml.ri"
_sof_fw_data_end:

.section __DATA,__sof_fwsz
_sof_fw_size:
.quad _sof_fw_data_end - _sof_fw_data
