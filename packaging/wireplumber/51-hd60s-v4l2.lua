-- iso_capture escribe YUYV 1080p60 en /dev/video42. Si PipeWire abre
-- el loopback como cámara, fija 640x480 y OBS no puede usarlo como V4L2.
v4l2_monitor.rules = v4l2_monitor.rules or {}
table.insert(v4l2_monitor.rules, {
  matches = {
    {
      { "node.name", "matches", "v4l2_input._sys_devices_virtual_video4linux_video42" },
    },
    {
      { "device.name", "matches", "v4l2_device._sys_devices_virtual_video4linux_video42" },
    },
    {
      { "api.v4l2.path", "equals", "/dev/video42" },
    },
  },
  apply_properties = {
    ["node.disabled"] = true,
    ["device.disabled"] = true,
  },
})
