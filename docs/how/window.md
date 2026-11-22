# The window

A session shows a host window: the cluster, both multimedia screens, the steering wheel and the vehicle controls side by side. The window uses statically linked SDL3 with Dear ImGui; the reasons are in [the SDL3 window](../why/sdl3-window.md).

## Screens

The cluster and the primary and secondary multimedia screens appear at their guest resolution of 1920×720 each, scaled together with the window. Pointer input on a multimedia screen reaches the Android guest on that screen.

The other screens of the vehicle appear in a separate screens window: the rear screen of Free 6.11.1 and 7.3.1 at 1902×540 and the second QNX screen of Passion at 1280×720. The profile describes each screen with its [screen role](../GLOSSARY.md), guest and size, and the window, frames, screenshots and the [Voyemu API](api.md) take the list from the machine. The screens window keeps the zoom of the window and shows its screens side by side; pointer input on the rear screen reaches the Android guest on that screen.

## Startup screen

Until Android is ready, each multimedia screen shows the OEM boot animation of the firmware, as in the vehicle: the primary screen plays `/system/media/bootanimation.zip`, the secondary screen `/system/media/bootanimation1.zip` and the rear screen of Free 6.11.1 and 7.3.1 `/system/media/bootanimation2.zip`. The parts play in the order of their `desc.txt` at its frame rate, and the last frame stays until Android is ready. [Preparation](preparation.md) imports those of these files that the profile declares with the other OEM inputs and copies each into the machine's `ui/startup/`. A screen without its own animation in a profile that has one stays black; a machine without imported animations shows the Voyemu logo on both screens. Android itself starts without a boot animation, so its frames never show the placeholder of the emulator image.

When the firmware has OEM startup videos, all of them start together each time the multimedia screens enter the startup screen, as the vehicle plays them at power-on: once, without sound, each limited to the time of the OEM player, and Android readiness stops them earlier. A multimedia screen plays its video before its boot animation, and the cluster plays its video in the cluster area before the usual cluster startup. A screen of the screens window plays its video in its area; the `hud` screen of Passion stays black after it, because its QNX output is not emulated yet. `build` decodes the videos from the imported QNX image into frames of their original resolution in the boot animation format, `ui/startup/display0-video.zip`, `ui/startup/display1-video.zip`, `ui/startup/cluster.zip` and `ui/startup/hud-video.zip`; the window scales them to the screens like the other frames. Decoding uses the H.264 decoder of the host operating system; the reasons are in [host video decoders](../why/host-video-decoders.md). Without a decoder, or when it rejects a video, that video phase stays black for the length of the video, and the rest of the startup is unchanged.

## Steering wheel

The steering wheel is drawn from the artwork of the profile, copied into the machine's `ui/wheel/`. Its buttons send the OEM steering-wheel events to the guests: media, volume, voice assistant and the other buttons of the vehicle line. The routes of the buttons come from the machine's `ui/wheel-routes.json`.

## Fonts

The window draws its text with the fonts of the vehicle and embeds none of its own. As in the apps of the vehicle, Latin, Cyrillic and Chinese text uses DFP King Gothic GB: Regular for ordinary text and Semibold for headings and choices. Characters that DFP King Gothic GB lacks, such as « and », come from Roboto. Each font is scaled so that its capital letters have the same height, so text has the same size on every profile. `build` copies the fonts that the [profile](profiles-machines.md#profile) declares into the machine's `ui/fonts/`. The Russian letters of DFP King Gothic GB Regular and Semibold advance a full em in the firmware, which spaces Russian text out; `build` writes these two fonts with the advances of the Russian letters reduced to their glyphs. A machine without these fonts stops the window with an error asking to rebuild the machine.

## Controls next to the wheel

Controls next to the wheel replace a menu:

- switch the zoom between 50, 75 and 100 percent;
- switch the interface language between Chinese, English and Russian;
- power each guest off and on and restart it;
- open and close the screens window, on a machine with more screens;
- open About with the release date, the website and the embedded licenses.

The interface language is shared by all machines and remembered; until a language is chosen, the window follows the preferred language of the host. Command output has its own language, see [launch and sessions](launch.md#output-language).

## Vehicle controls

A panel of vehicle controls changes the [vehicle model](vehicle.md): driving values such as speed and gear, doors, lights, tyres, temperatures, charge and fuel. A list of all fields of the catalog of the machine allows any value to be changed directly; the controls of a field that reaches neither guest are grayed out as a profile limit. Voyemu draws the switches, the choices and the panel background itself, the same for every profile, in the light or dark theme of the multimedia.

## Background launch

`start --background` keeps the window and its audio behind other windows, so a launch does not take focus. This suits automation that drives the session through the [Voyemu API](api.md).
