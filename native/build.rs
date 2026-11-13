fn main() {
    println!("cargo:rerun-if-changed=nvenc.c");
    println!("cargo:rerun-if-changed=hdr_probe.c");
    println!("cargo:rerun-if-changed=gpu_interop.c");
    println!("cargo:rerun-if-changed=gpu_interop.h");
    println!("cargo:rerun-if-changed=gpu_kernel_ptx.h");
    println!("cargo:rerun-if-env-changed=WAYBRIDGE_CUDA_ROOT");
    println!("cargo:rustc-link-lib=dl");
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
    let cuda = std::path::PathBuf::from(
        std::env::var_os("WAYBRIDGE_CUDA_ROOT").unwrap_or_else(|| "/opt/cuda".into()),
    );
    if cuda.join("include/cuda.h").is_file() && cuda.join("include/cudaGL.h").is_file() {
        build
            .include(cuda.join("include"))
            .define("WB_CUDA_INTEROP", "1");
    }
    build
        .file("gpu_interop.c")
        .file("nvenc.c")
        .file("hdr_probe.c")
        .warnings(true)
        .compile("waybridge_nvenc");
}
