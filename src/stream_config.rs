use crate::*;

/// The CLI and live configuration resolve the same policy before touching an
/// encoder. HDR remains capability-gated separately; this selects a base codec.
pub fn resolve_codec(requested: &str, hdr: bool) -> Result<Option<waybridge_codec::Codec>> {
    use waybridge_codec::Codec;
    Ok(match requested {
        "png" => None,
        "auto" if hdr => Some(Codec::Hevc),
        "auto" | "h264" | "h264-nvenc" => Some(Codec::H264),
        "hevc" | "hevc-nvenc" => Some(Codec::Hevc),
        "av1" | "av1-nvenc" => Some(Codec::Av1),
        _ => bail!("unsupported codec"),
    })
}

fn codec_name(codec: Option<waybridge_codec::Codec>) -> &'static str {
    use waybridge_codec::Codec;
    match codec {
        None => "png",
        Some(Codec::H264) => "h264",
        Some(Codec::Hevc | Codec::HevcHdr) => "hevc",
        Some(Codec::Av1 | Codec::Av1Hdr) => "av1",
    }
}

impl App {
    pub fn stream_config(&mut self, value: &Value) {
        let request = value["request_id"].as_u64().unwrap_or(0);
        let fps = value["fps"].as_u64().unwrap_or(0);
        let bitrate = value["bitrate"].as_u64().unwrap_or(0);
        let requested = value["codec"].as_str().unwrap_or("");
        let hdr = value["hdr"].as_bool().unwrap_or(self.hdr_requested);
        if let Err(error) = self
            .stream_limits
            .validate_stream(fps, bitrate, requested, hdr)
        {
            self.control(
                json!({"type":"stream_config","request_id":request,"ok":false,
                "message":error.to_string(),"limits":self.stream_limits}),
            );
            return;
        }
        let codec = match resolve_codec(requested, hdr) {
            Ok(codec) => codec,
            Err(_) => {
                self.control(json!({"type":"stream_config","request_id":request,"ok":false,"message":"unsupported codec"}));
                return;
            }
        };
        let video_limited = !self.video_allowed && codec.is_some();
        let codec = if self.video_allowed { codec } else { None };
        let hdr_changed = self.hdr_requested != hdr;
        self.hdr_requested = hdr;
        let read_only = value["read_only"].as_bool().unwrap_or(false);
        if read_only && !self.read_only {
            self.cancel_text_drag();
            self.cancel_x11_drop();
            self.cancel_window_drag();
            self.release_input();
        }
        self.read_only = read_only;
        if let Some(peer) = &self.peer {
            peer.set_desktop_input(!read_only);
        }
        if self.stream_fps != fps as u32
            || self.stream_bitrate != bitrate as u32
            || self.video_codec != codec
            || hdr_changed
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
        self.control(
            json!({"type":"stream_config","request_id":request,"ok":true,
            "fps":self.stream_fps,"bitrate":self.stream_bitrate,"codec":codec_name(codec),
            "requested_codec":requested,"hdr_requested":hdr,
            "fallback_reason":video_limited.then_some("session_video_disabled"),
            "source":"runtime_request","persisted":false,"limits":self.stream_limits,
            "read_only":self.read_only}),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn startup_and_live_codec_policy_agree_for_hdr_and_sdr() {
        use waybridge_codec::Codec;
        assert_eq!(resolve_codec("auto", false).unwrap(), Some(Codec::H264));
        assert_eq!(resolve_codec("auto", true).unwrap(), Some(Codec::Hevc));
        for (cli, live, expected) in [
            ("h264-nvenc", "h264", Codec::H264),
            ("hevc-nvenc", "hevc", Codec::Hevc),
            ("av1-nvenc", "av1", Codec::Av1),
        ] {
            for hdr in [false, true] {
                assert_eq!(resolve_codec(cli, hdr).unwrap(), Some(expected));
                assert_eq!(
                    resolve_codec(cli, hdr).unwrap(),
                    resolve_codec(live, hdr).unwrap()
                );
            }
        }
        assert_eq!(resolve_codec("png", true).unwrap(), None);
        assert!(resolve_codec("unknown", false).is_err());
    }
}
