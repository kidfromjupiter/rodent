# Rodent
![tempFileForShare_20260403-231814](https://github.com/user-attachments/assets/b97edbd5-0fb9-404b-a1fc-3b9db1c81441)
*(drawn using this software on samsung notes)*

## What is it?
A software that mimics the functionality of a hardware KVM, which would allow you to control multiple devices with 1 set of HID devices

## Why does it exist when there are alternitives?
Most other alternatives work via WIFI LAN. Which introduces latency. And a LAN connection may not always be possible. This software aims to solve it by emulataing a bluetooth HID device. This means no third party software 
is required on other devices, except of course, the host device ( which needs to be running rodent ). Additional functionality, such as clipboard share, may require third party software.

## How does it work?
It turns your computer into a literal bluetooth mouse and passes through HID events. That's it. No frills. No fluff. Clipboard share is supported between wayland and android though. 
I see clipboard share as something essential for a true connected experience. 

## How to use
1. Install rodent on host machine
2. Run rodent
3. Connect to your PC via bluetooth. ( disable all other profiles such as audio, calls except for input if you see this )
4. Ctrl + Escape to toggle input grabbing. 

## Troubleshooting input device permissions
Rodent needs access to `/dev/input/eventX` devices. If you see errors like:

- `Failed to find keyboard evdev device (requested path: /dev/input/event3)`
- `Failed to find mouse evdev device (requested path: /dev/input/event15)`

add your user to the `input` group:

`sudo usermod -aG input $USER`

## Known limitations
- Currently supports switching to only 1 other device. This may improve in the future. But since we're at the mercy of bluetooth procotols, its hard to say till I find out more
