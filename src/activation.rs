//! Activation is a short-lived user intent, separate from window visibility and
//! compositor keyboard focus. Windows confirms successful focus through `focus`.
use super::*;
use smithay::wayland::xdg_activation::{
    XdgActivationHandler, XdgActivationState, XdgActivationToken, XdgActivationTokenData,
};
use std::collections::{BTreeSet, HashMap};

const LIFETIME: Duration = Duration::from_secs(15);
#[derive(Clone)]
struct Intent {
    epoch: u64,
    origin: Option<u32>,
    request: Option<String>,
    created: Instant,
}
struct Launch {
    intent: Intent,
    app: String,
    pid: Option<u32>,
    bound: bool,
    requests_only: bool,
}
pub(crate) struct Activation {
    pub xdg: XdgActivationState,
    pub negotiated: bool,
    epoch: u64,
    launches: HashMap<String, Launch>,
    pending: BTreeMap<u32, Intent>,
    seen: BTreeSet<u32>,
    pub focus_serial: Option<(Serial, u32, Instant)>,
}
impl Activation {
    pub fn new(dh: &DisplayHandle) -> Self {
        Self {
            xdg: XdgActivationState::new::<App>(dh),
            negotiated: false,
            epoch: 0,
            launches: HashMap::new(),
            pending: BTreeMap::new(),
            seen: BTreeSet::new(),
            focus_serial: None,
        }
    }
    pub fn connection(&mut self) {
        self.epoch += 1;
        self.negotiated = false;
        self.launches.clear();
        self.pending.clear();
        self.focus_serial = None;
        self.xdg.retain_tokens(|_, _| false);
    }
    pub fn unmap(&mut self, id: u32) {
        self.seen.remove(&id);
        self.pending.remove(&id);
    }
}
fn identifier(value: &str) -> bool {
    value.len() <= 256 && !value.chars().any(char::is_control)
}
fn request_id(value: &str) -> bool {
    value.len() == 32 && value.bytes().all(|b| b.is_ascii_hexdigit())
}
fn app_match(app: &str, desktop: &str) -> bool {
    !desktop.is_empty() && app == desktop.strip_suffix(".desktop").unwrap_or(desktop)
}
fn descendant(mut child: u32, parent: u32) -> bool {
    for _ in 0..16 {
        if child == parent {
            return true;
        }
        let Ok(stat) = fs::read_to_string(format!("/proc/{child}/stat")) else {
            break;
        };
        let Some(next) = stat
            .rsplit_once(')')
            .and_then(|(_, tail)| tail.split_whitespace().nth(1))
            .and_then(|v| v.parse::<u32>().ok())
        else {
            break;
        };
        if next <= 1 || next == child {
            break;
        }
        child = next;
    }
    false
}
impl App {
    // The SNI worker integration is delivered independently of this module.
    #[allow(dead_code)]
    pub(crate) fn activation_for_owner(&mut self, request: &str, pid: u32) -> bool {
        let replaced: Vec<_> = self
            .activation
            .launches
            .iter()
            .filter(|(_, launch)| {
                launch.intent.created.elapsed() >= LIFETIME
                    || (launch.requests_only && launch.pid == Some(pid))
            })
            .map(|(token, _)| token.clone())
            .collect();
        for token in replaced {
            self.activation.launches.remove(&token);
            self.activation.xdg.remove_token(&token.into());
        }
        if !self.activation.negotiated
            || self.read_only
            || !request_id(request)
            || pid <= 1
            || self.activation.launches.len() >= 32
        {
            return false;
        }
        let intent = Intent {
            epoch: self.activation.epoch,
            origin: None,
            request: Some(request.into()),
            created: Instant::now(),
        };
        let (token, _) = self.activation.xdg.create_external_token(None);
        self.activation.launches.insert(
            token.as_str().into(),
            Launch {
                intent,
                app: String::new(),
                pid: Some(pid),
                bound: true,
                requests_only: true,
            },
        );
        true
    }
    fn activation_owner_intent(&mut self, pid: u32) -> Option<Intent> {
        let token = self
            .activation
            .launches
            .iter()
            .find(|(_, launch)| {
                launch.requests_only
                    && launch.intent.created.elapsed() < LIFETIME
                    && launch.pid.is_some_and(|owner| descendant(pid, owner))
            })
            .map(|(token, _)| token.clone())?;
        let launch = self.activation.launches.remove(&token)?;
        self.activation.xdg.remove_token(&token.into());
        Some(launch.intent)
    }
    pub(crate) fn activation_x11(&mut self, window: smithay::xwayland::xwm::X11Surface) {
        let Some(id) = self
            .windows
            .iter()
            .find(|w| w.window.x11_surface() == Some(&window))
            .map(|w| w.id)
        else {
            return;
        };
        let startup = window.startup_id();
        if let Some(launch) = startup
            .as_ref()
            .and_then(|t| self.activation.launches.remove(t))
        {
            self.activation.xdg.remove_token(&startup.unwrap().into());
            self.activation_queue(id, launch.intent);
            return;
        }
        if let Some(intent) = window
            .pid()
            .and_then(|pid| self.activation_owner_intent(pid))
        {
            self.activation_queue(id, intent);
            return;
        }
        let origin = self.focused_window.filter(|focused| {
            self.window(*focused)
                .and_then(|w| w.x11_surface().and_then(|s| s.pid()))
                .zip(window.pid())
                .is_some_and(|(a, b)| a == b)
        });
        self.activation_queue(
            id,
            Intent {
                epoch: self.activation.epoch,
                origin,
                request: None,
                created: Instant::now(),
            },
        );
    }
    pub(crate) fn activation_message(&mut self, value: &Value) -> bool {
        match value["type"].as_str() {
            Some("activation_prepare") if self.activation.negotiated => {
                let Some(request) = value["request_id"].as_str().filter(|s| request_id(s)) else {
                    return true;
                };
                let app = value["app"].as_str().unwrap_or("");
                if !identifier(app) || self.activation.launches.len() >= 32 {
                    return true;
                }
                let intent = Intent {
                    epoch: self.activation.epoch,
                    origin: self.focused_window,
                    request: Some(request.into()),
                    created: Instant::now(),
                };
                let (token, _) = self.activation.xdg.create_external_token(None);
                let token = token.as_str().to_owned();
                self.activation.launches.insert(
                    token.clone(),
                    Launch {
                        intent,
                        app: app.into(),
                        pid: None,
                        bound: false,
                        requests_only: false,
                    },
                );
                self.control(
                    json!({"type":"activation_prepared","request_id":request,"token":token}),
                );
                true
            }
            Some("activation_launch") if self.activation.negotiated => {
                let request = value["request_id"].as_str().unwrap_or("");
                for launch in self
                    .activation
                    .launches
                    .values_mut()
                    .filter(|l| l.intent.request.as_deref() == Some(request))
                {
                    launch.pid = value["pid"]
                        .as_u64()
                        .and_then(|p| u32::try_from(p).ok())
                        .filter(|p| *p > 1);
                    launch.bound = true;
                }
                true
            }
            _ => false,
        }
    }
    fn activation_queue(&mut self, id: u32, intent: Intent) {
        if intent.epoch != self.activation.epoch
            || intent.created.elapsed() >= LIFETIME
            || self.read_only
        {
            return;
        }
        if self.activation.negotiated {
            self.activation.pending.insert(id, intent);
        } else {
            self.focus(Some(id));
        }
    }
    pub(crate) fn activation_flush(&mut self) {
        let epoch = self.activation.epoch;
        self.activation
            .xdg
            .retain_tokens(|_, data| data.timestamp.elapsed() < LIFETIME);
        self.activation
            .launches
            .retain(|_, l| l.intent.epoch == epoch && l.intent.created.elapsed() < LIFETIME);
        self.activation
            .pending
            .retain(|_, i| i.epoch == epoch && i.created.elapsed() < LIFETIME);
        if !self.activation.negotiated {
            return;
        }
        let mapped: Vec<_> = self
            .windows
            .iter()
            .filter(|w| {
                w.mapped && w.window.alive() && w.announced && w.last_info["type"] == "window"
            })
            .map(|w| {
                (
                    w.id,
                    w.last_info["app_id"].as_str().unwrap_or("").to_owned(),
                    w.last_info["parent"]
                        .as_u64()
                        .and_then(|p| u32::try_from(p).ok()),
                    w.window.x11_surface().and_then(|x| x.pid()).or_else(|| {
                        w.window
                            .wl_surface()
                            .and_then(|s| s.client())
                            .and_then(|c| c.get_credentials(&self.dh).ok())
                            .map(|c| c.pid as u32)
                    }),
                    w.window.x11_surface().and_then(|x| x.startup_id()),
                )
            })
            .collect();
        for (id, app, parent, pid, startup) in mapped {
            let fresh = self.activation.seen.insert(id);
            let token = self
                .activation
                .launches
                .iter()
                .find(|(token, l)| {
                    l.bound
                        && (!l.requests_only || fresh)
                        && (startup.as_deref() == Some(token.as_str())
                            || (l.pid.is_none() && app_match(&app, &l.app))
                            || l.pid
                                .zip(pid)
                                .is_some_and(|(parent, child)| descendant(child, parent)))
                })
                .map(|(t, _)| t.clone());
            if let Some(token) = token {
                let launch = self.activation.launches.remove(&token).unwrap();
                self.activation.xdg.remove_token(&token.into());
                self.activation_queue(id, launch.intent);
            } else if fresh && !self.activation.pending.contains_key(&id) {
                // A transient of the currently focused application inherits only
                // its parent's foreground privilege; another app cannot claim it.
                let launched_child = self.focused_window.filter(|focused| {
                    self.last_interaction.is_some_and(|(_, source, time)| {
                        source == *focused && time.elapsed() < Duration::from_secs(5)
                    }) && pid.is_some_and(|child| {
                        self.window(*focused)
                            .and_then(|w| {
                                w.x11_surface().and_then(|x| x.pid()).or_else(|| {
                                    w.wl_surface()
                                        .and_then(|s| s.client())
                                        .and_then(|c| c.get_credentials(&self.dh).ok())
                                        .map(|c| c.pid as u32)
                                })
                            })
                            .is_some_and(|parent| child != parent && descendant(child, parent))
                    })
                });
                if (parent.is_some() && parent == self.focused_window) || launched_child.is_some() {
                    self.activation_queue(
                        id,
                        Intent {
                            epoch,
                            origin: parent.or(launched_child),
                            request: None,
                            created: Instant::now(),
                        },
                    );
                }
            }
            if let Some(intent) = self.activation.pending.remove(&id) {
                self.control(json!({"type":"window_activate","id":self.modal_target(id),"origin":intent.origin,"request_id":intent.request}));
            }
        }
        self.activation
            .seen
            .retain(|id| self.windows.iter().any(|w| w.id == *id));
    }
}
impl XdgActivationHandler for App {
    fn activation_state(&mut self) -> &mut XdgActivationState {
        &mut self.activation.xdg
    }
    fn token_created(&mut self, _: XdgActivationToken, data: XdgActivationTokenData) -> bool {
        if self.activation.xdg.tokens().count() > 128 {
            return false;
        }
        if let Some(intent) = data
            .client_id
            .clone()
            .and_then(|id| self.dh.backend_handle().get_client_credentials(id).ok())
            .and_then(|credentials| self.activation_owner_intent(credentials.pid as u32))
        {
            data.user_data.insert_if_missing(|| intent);
            return true;
        }
        let Some((serial, seat)) = data.serial.as_ref() else {
            return false;
        };
        if Seat::<App>::from_resource(seat).as_ref() != Some(&self.seat) {
            return false;
        }
        let valid = [self.last_interaction, self.activation.focus_serial]
            .into_iter()
            .flatten()
            .find(|(s, id, time)| {
                s == serial
                    && Some(*id) == self.focused_window
                    && time.elapsed() < Duration::from_secs(5)
            });
        let Some((_, id, _)) = valid else {
            return false;
        };
        let source = self
            .window(id)
            .and_then(|w| w.wl_surface().map(|s| s.into_owned()));
        if data.client_id != source.as_ref().and_then(|s| s.client()).map(|c| c.id()) {
            return false;
        }
        data.user_data.insert_if_missing(|| Intent {
            epoch: self.activation.epoch,
            origin: Some(id),
            request: None,
            created: Instant::now(),
        });
        true
    }
    fn request_activation(
        &mut self,
        token: XdgActivationToken,
        data: XdgActivationTokenData,
        surface: WlSurface,
    ) {
        self.activation.xdg.remove_token(&token);
        let intent = self
            .activation
            .launches
            .remove(token.as_str())
            .map(|l| l.intent)
            .or_else(|| data.user_data.get::<Intent>().cloned());
        let Some(intent) = intent.filter(|_| data.timestamp.elapsed() < LIFETIME) else {
            return;
        };
        if let Some(id) = self
            .windows
            .iter()
            .find(|w| w.window.wl_surface().as_deref() == Some(&surface))
            .map(|w| w.id)
        {
            self.activation_queue(id, intent);
        }
    }
}
smithay::delegate_xdg_activation!(App);
