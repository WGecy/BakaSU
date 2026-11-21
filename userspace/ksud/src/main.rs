#![feature(decl_macro)]

#[cfg(target_os = "android")]
mod android;
#[cfg(target_os = "android")]
mod anykernel3;
mod apk_sign;
mod assets;
mod banner;
mod boot_patch;
#[cfg(not(target_os = "android"))]
mod cli_non_android;
mod defs;
mod feature_id;
mod lkm_image;
mod lkm_image_btf;
#[cfg(target_os = "android")]
mod magic_mount;
#[cfg(target_os = "android")]
mod mount_capture;
#[cfg(any(target_os = "android", test))]
#[cfg_attr(not(target_os = "android"), allow(dead_code))]
mod mount_guard;
#[cfg_attr(not(target_os = "android"), allow(dead_code))]
mod mount_plan;
#[cfg_attr(not(target_os = "android"), allow(dead_code))]
mod mount_ui;
#[cfg(any(target_os = "android", test))]
#[cfg_attr(not(target_os = "android"), allow(dead_code))]
mod nomount;
#[cfg(any(target_os = "android", test))]
#[cfg_attr(not(target_os = "android"), allow(dead_code))]
mod nomount_plan;
#[cfg(any(target_os = "android", test))]
mod startup_epoch;

fn main() -> anyhow::Result<()> {
    #[cfg(target_os = "android")]
    {
        android::cli::run()
    }
    #[cfg(not(target_os = "android"))]
    {
        cli_non_android::run()
    }
}
