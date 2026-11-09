fn main() {
    println!("cargo:rerun-if-changed=nvenc.c");
    let avcodec = pkg_config::Config::new()
        .probe("libavcodec")
        .expect("FFmpeg libavcodec development files");
    let avutil = pkg_config::Config::new()
        .probe("libavutil")
        .expect("FFmpeg libavutil development files");
    let mut build = cc::Build::new();
    for path in avcodec.include_paths.iter().chain(&avutil.include_paths) {
        build.include(path);
    }
    build
        .file("nvenc.c")
        .warnings(true)
        .compile("waybridge_nvenc");
}
