# External apps

External apps are not part of the firmware: they are installed on the phone separately, one by
one, and only if you want them. Once installed, an external app shows up in the menu among the
others, in its section, and works like the built-in apps.

| App | What it does |
|---|---|
| [Telegram](telegram.md) | Your Telegram chats, replies, articles in Instant View, notifications |
| [GitHub](github.md) | Your GitHub notifications, pull requests, issues and repositories |
| **Browser** | Web pages with JavaScript, for the sites [Web Lite](../apps/web-lite.md) cannot show |
| **Terminal** | A shell on the phone |
| **Doom**, **Genesis** | Games: Doom, and Sega Mega Drive / Genesis games |

## Installing an app

External apps are installed from the computer the firmware was built on, with the phone
connected over USB and the computer allowed on it (see
[Computers on USB](../index.md#computers-on-usb)). In the firmware's folder, run:

```
just app telegram
```

with the app's name in lower case (`telegram`, `github`, `web` for Browser, `terminal`, `doom`,
`genesis`). The phone's interface restarts and opens the new app. `just apps` installs all of
them at once, and `just app-rm telegram` removes one.

## Kept across updates

Installed apps live with your data on the phone, not in the firmware: updating the firmware
keeps them, their settings and what they downloaded.

!!! warning "An app that no longer fits the firmware"
    An update that changes what the apps are built on leaves out the apps installed before it:
    they no longer show in the menu, but their data stays. Install them again (`just app` as
    above) after updating the firmware with such a change.
