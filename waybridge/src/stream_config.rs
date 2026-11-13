use crate::*;

impl App {
    pub fn stream_config(&mut self, value: &Value) {
        let request = value["request_id"].as_u64().unwrap_or(0);
        let fps = value["fps"].as_u64().unwrap_or(0);
        let bitrate = value["bitrate"].as_u64().unwrap_or(0);
        let requested = value["codec"].as_str().unwrap_or("");
        let codec = match requested {
            "png" => None,
            "auto" | "h264" => Some(waybridge_codec::Codec::H264),
            "hevc" => Some(waybridge_codec::Codec::Hevc),
            "av1" => Some(waybridge_codec::Codec::Av1),
            _ => {
                self.control(json!({"type":"stream_config","request_id":request,"ok":false,"message":"unsupported codec"}));
                return;
            }
        };
        if !(1..=120).contains(&fps) || !(1000..=200000).contains(&bitrate) {
            self.control(json!({"type":"stream_config","request_id":request,"ok":false,"message":"invalid frame rate or bitrate"}));
            return;
        }
        let codec = if self.video_allowed { codec } else { None };
        let read_only = value["read_only"].as_bool().unwrap_or(false);
        if read_only && !self.read_only {
            self.cancel_window_drag();
            self.release_input();
        }
        self.read_only = read_only;
        if self.stream_fps != fps as u32
            || self.stream_bitrate != bitrate as u32
            || self.video_codec != codec
        {
            self.stream_fps = fps as u32;
            self.stream_bitrate = bitrate as u32;
            self.video_codec = codec;
            self.encoder.reconfigure(codec, fps as u32, bitrate as u32);
            for window in &mut self.windows {
                window.dirty = true;
            }
            for popup in &mut self.remote_popups {
                popup.dirty = true;
            }
        }
        self.control(json!({"type":"stream_config","request_id":request,"ok":true,
            "fps":self.stream_fps,"bitrate":self.stream_bitrate,"codec":if codec.is_none() {"png"}else{requested},
            "read_only":self.read_only}));
    }
}
