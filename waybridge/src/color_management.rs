//! Experimental v1 Windows-scRGB subset. ICC and parametric descriptions are
//! deliberately not advertised. Preferred/output descriptions remain honest SDR.
use crate::App;
use smithay::{
    output::Output,
    reexports::{
        wayland_protocols::wp::color_management::v1::server::{
            wp_color_management_output_v1::{self as output, WpColorManagementOutputV1},
            wp_color_management_surface_feedback_v1::{
                self as feedback, WpColorManagementSurfaceFeedbackV1,
            },
            wp_color_management_surface_v1::{self as surface, WpColorManagementSurfaceV1},
            wp_color_manager_v1::{self as manager, WpColorManagerV1},
            wp_image_description_creator_icc_v1::{self as icc, WpImageDescriptionCreatorIccV1},
            wp_image_description_creator_params_v1::{
                self as params, WpImageDescriptionCreatorParamsV1,
            },
            wp_image_description_info_v1::WpImageDescriptionInfoV1,
            wp_image_description_v1::{self as description, WpImageDescriptionV1},
        },
        wayland_server::{
            backend::GlobalId, protocol::wl_surface::WlSurface, Client, DataInit, Dispatch,
            DisplayHandle, GlobalDispatch, New, Resource, WEnum,
        },
    },
    wayland::compositor::{with_states, Cacheable},
};
use std::sync::atomic::{AtomicBool, Ordering};
#[derive(Clone, Copy, Default)]
pub struct SurfaceColor {
    pub scrgb: bool,
}
impl Cacheable for SurfaceColor {
    fn commit(&mut self, _: &DisplayHandle) -> Self {
        *self
    }
    fn merge_into(self, into: &mut Self, _: &DisplayHandle) {
        *into = self;
    }
}
#[derive(Default)]
struct Attached(AtomicBool);
pub struct SurfaceData {
    surface: WlSurface,
}
pub struct OutputData {
    output: Option<Output>,
}
#[derive(Clone, Copy)]
pub struct DescriptionData {
    scrgb: bool,
    information: bool,
    ready: bool,
}
pub fn register(dh: &DisplayHandle) -> GlobalId {
    dh.create_global::<App, WpColorManagerV1, _>(1, ())
}

fn make_description(
    id: New<WpImageDescriptionV1>,
    data_init: &mut DataInit<'_, App>,
    scrgb: bool,
    information: bool,
    ready: bool,
) -> WpImageDescriptionV1 {
    let description = data_init.init(
        id,
        DescriptionData {
            scrgb,
            information,
            ready,
        },
    );
    if ready {
        description.ready(if scrgb { 2 } else { 1 });
    } else {
        description.failed(
            description::Cause::NoOutput,
            "the output no longer exists".into(),
        );
    }
    description
}
impl GlobalDispatch<WpColorManagerV1, ()> for App {
    fn bind(
        _: &mut Self,
        _: &DisplayHandle,
        _: &Client,
        resource: New<WpColorManagerV1>,
        _: &(),
        data_init: &mut DataInit<'_, Self>,
    ) {
        let manager = data_init.init(resource, ());
        manager.supported_intent(manager::RenderIntent::Perceptual);
        manager.supported_feature(manager::Feature::WindowsScrgb);
        manager.done();
    }
}
impl Dispatch<WpColorManagerV1, ()> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpColorManagerV1,
        request: manager::Request,
        _: &(),
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            manager::Request::Destroy => {}
            manager::Request::CreateWindowsScrgb { image_description } => {
                make_description(image_description, data_init, true, false, true);
            }
            manager::Request::GetSurface { id, surface } => {
                let attached = with_states(&surface, |states| {
                    states
                        .data_map
                        .insert_if_missing_threadsafe(Attached::default);
                    states
                        .data_map
                        .get::<Attached>()
                        .unwrap()
                        .0
                        .swap(true, Ordering::SeqCst)
                });
                data_init.init(id, SurfaceData { surface });
                if attached {
                    resource.post_error(
                        manager::Error::SurfaceExists,
                        "surface already has color management",
                    );
                }
            }
            manager::Request::GetSurfaceFeedback { id, surface } => {
                let feedback = data_init.init(id, SurfaceData { surface });
                feedback.preferred_changed(1);
            }
            manager::Request::GetOutput { id, output } => {
                data_init.init(
                    id,
                    OutputData {
                        output: Output::from_resource(&output),
                    },
                );
            }
            manager::Request::CreateIccCreator { obj } => {
                data_init.init(obj, ());
                resource.post_error(
                    manager::Error::UnsupportedFeature,
                    "ICC profiles are not supported by the experimental scRGB subset",
                );
            }
            manager::Request::CreateParametricCreator { obj } => {
                data_init.init(obj, ());
                resource.post_error(
                    manager::Error::UnsupportedFeature,
                    "only the predefined Windows-scRGB creator is supported",
                );
            }
            _ => resource.post_error(
                manager::Error::UnsupportedFeature,
                "unsupported color-management version/request",
            ),
        }
    }
}
impl Dispatch<WpColorManagementSurfaceV1, SurfaceData> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpColorManagementSurfaceV1,
        request: surface::Request,
        data: &SurfaceData,
        _: &DisplayHandle,
        _: &mut DataInit<'_, Self>,
    ) {
        if !data.surface.is_alive() {
            if !matches!(request, surface::Request::Destroy) {
                resource.post_error(surface::Error::Inert, "surface destroyed");
            }
            return;
        }
        match request {
            surface::Request::SetImageDescription {
                image_description,
                render_intent,
            } => {
                if render_intent != WEnum::Value(manager::RenderIntent::Perceptual) {
                    resource.post_error(
                        surface::Error::RenderIntent,
                        "only perceptual intent is supported",
                    );
                    return;
                }
                let Some(description) = image_description
                    .data::<DescriptionData>()
                    .filter(|description| description.ready)
                else {
                    resource.post_error(
                        surface::Error::ImageDescription,
                        "image description is not ready",
                    );
                    return;
                };
                with_states(&data.surface, |states| {
                    states.cached_state.get::<SurfaceColor>().pending().scrgb = description.scrgb
                });
            }
            surface::Request::UnsetImageDescription | surface::Request::Destroy => {
                with_states(&data.surface, |states| {
                    states.cached_state.get::<SurfaceColor>().pending().scrgb = false
                });
            }
            _ => {}
        }
    }
    fn destroyed(
        _: &mut Self,
        _: smithay::reexports::wayland_server::backend::ClientId,
        _: &WpColorManagementSurfaceV1,
        data: &SurfaceData,
    ) {
        if data.surface.is_alive() {
            with_states(&data.surface, |states| {
                if let Some(attached) = states.data_map.get::<Attached>() {
                    attached.0.store(false, Ordering::SeqCst);
                }
                states.cached_state.get::<SurfaceColor>().pending().scrgb = false;
            });
        }
    }
}
impl Dispatch<WpColorManagementOutputV1, OutputData> for App {
    fn request(
        state: &mut Self,
        _: &Client,
        _: &WpColorManagementOutputV1,
        request: output::Request,
        data: &OutputData,
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        if let output::Request::GetImageDescription { image_description } = request {
            let alive = data.output.as_ref().is_some_and(|output| {
                output == &state.output
                    || state.windows.iter().any(|window| &window.output == output)
            });
            make_description(image_description, data_init, false, true, alive);
        }
    }
}
impl Dispatch<WpColorManagementSurfaceFeedbackV1, SurfaceData> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpColorManagementSurfaceFeedbackV1,
        request: feedback::Request,
        data: &SurfaceData,
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            feedback::Request::GetPreferred { image_description } => {
                make_description(
                    image_description,
                    data_init,
                    false,
                    true,
                    data.surface.is_alive(),
                );
                if !data.surface.is_alive() {
                    resource.post_error(feedback::Error::Inert, "surface destroyed");
                }
            }
            feedback::Request::GetPreferredParametric { image_description } => {
                make_description(image_description, data_init, false, true, false);
                resource.post_error(
                    feedback::Error::UnsupportedFeature,
                    "parametric descriptions are not supported",
                );
            }
            _ => {}
        }
    }
}
impl Dispatch<WpImageDescriptionV1, DescriptionData> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpImageDescriptionV1,
        request: description::Request,
        data: &DescriptionData,
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        if let description::Request::GetInformation { information } = request {
            let info = data_init.init(information, ());
            if !data.ready {
                resource.post_error(description::Error::NotReady, "description not ready");
                return;
            }
            if !data.information {
                resource.post_error(
                    description::Error::NoInformation,
                    "predefined scRGB descriptions cannot provide information",
                );
                return;
            }
            info.primaries(
                640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000,
            );
            info.primaries_named(manager::Primaries::Srgb);
            info.tf_named(manager::TransferFunction::Srgb);
            info.luminances(0, 80, 80);
            info.target_primaries(
                640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000,
            );
            info.target_luminance(0, 80);
            info.done();
        }
    }
}
impl Dispatch<WpImageDescriptionInfoV1, ()> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        _: &WpImageDescriptionInfoV1,
        request:smithay::reexports::wayland_protocols::wp::color_management::v1::server::wp_image_description_info_v1::Request,
        _: &(),
        _: &DisplayHandle,
        _: &mut DataInit<'_, Self>,
    ) {
        let _ = request;
    }
}
impl Dispatch<WpImageDescriptionCreatorIccV1, ()> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpImageDescriptionCreatorIccV1,
        request: icc::Request,
        _: &(),
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        if let icc::Request::Create { image_description } = request {
            make_description(image_description, data_init, false, false, false);
        }
        resource.post_error(icc::Error::IncompleteSet, "ICC creator was not advertised");
    }
}
impl Dispatch<WpImageDescriptionCreatorParamsV1, ()> for App {
    fn request(
        _: &mut Self,
        _: &Client,
        resource: &WpImageDescriptionCreatorParamsV1,
        request: params::Request,
        _: &(),
        _: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        if let params::Request::Create { image_description } = request {
            make_description(image_description, data_init, false, false, false);
        }
        resource.post_error(
            params::Error::UnsupportedFeature,
            "parametric creator was not advertised",
        );
    }
}
