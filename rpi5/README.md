# norns on Raspberry Pi 5 — KY-040 encoders & keys

Three KY-040 rotary encoder modules replace the stock norns encoders *and*
keys: each KY-040 has a built-in push switch, so pushing encoder *N* is
norns key *N*.

No matron source changes are needed. A device-tree overlay
([`norns-ky040-overlay.dts`](norns-ky040-overlay.dts)) makes the kernel do the
quadrature decoding and debouncing, exposing the same style of
`/dev/input` devices the stock norns overlay provides; matron's existing
`keys:gpio` / `enc:gpio` backends read them (see `matronrc.lua`).

## Wiring

KY-040 breakout pins: `GND`, `+` (VCC), `SW` (push switch), `DT` (channel B),
`CLK` (channel A).

**Power `+` from 3V3 only — never 5V.** CLK/DT idle at the module's `+` rail
through its onboard pull-ups, and Pi GPIOs are not 5V-tolerant.

```
Raspberry Pi 5 (40-pin header, BCM numbers)         3× KY-040

      3V3  (pin 1)  o───────┬──────────┬──────────┐
                            │          │          │
                          [ + ]      [ + ]      [ + ]
   GPIO4   (pin 7)  o────[ CLK ]       │          │
   GPIO17  (pin 11) o────[ DT  ]       │          │
   GPIO22  (pin 15) o────[ SW  ]       │          │
                         ENC 1 / K1    │          │
                                       │          │
   GPIO5   (pin 29) o──────────────[ CLK ]        │
   GPIO6   (pin 31) o──────────────[ DT  ]        │
   GPIO13  (pin 33) o──────────────[ SW  ]        │
                                   ENC 2 / K2     │
                                                  │
   GPIO16  (pin 36) o─────────────────────────[ CLK ]
   GPIO26  (pin 37) o─────────────────────────[ DT  ]
   GPIO12  (pin 32) o─────────────────────────[ SW  ]
                                              ENC 3 / K3
                          [ GND ]    [ GND ]    [ GND ]
                            │          │          │
      GND  (pin 9)  o───────┴──────────┴──────────┘
                    (any GND pin works: 6, 9, 14, 20, 25, 30, 34, 39)
```

| Encoder | Function  | CLK (A)        | DT (B)         | SW             |
|---------|-----------|----------------|----------------|----------------|
| 1       | E1 / K1   | GPIO4  (pin 7) | GPIO17 (pin 11)| GPIO22 (pin 15)|
| 2       | E2 / K2   | GPIO5  (pin 29)| GPIO6  (pin 31)| GPIO13 (pin 33)|
| 3       | E3 / K3   | GPIO16 (pin 36)| GPIO26 (pin 37)| GPIO12 (pin 32)|

Notes:

- These pins don't conflict with the SSD1309 display
  (SPI0: GPIO8/9/10/11, D/C: 25, RST: 27, CS: 24) and leave I2C (2/3)
  and UART (14/15) free.
- The overlay enables internal pull-ups on all nine lines, so this also
  works with bare KY-040s (no breakout PCB); the breakout's own 10k
  pull-ups on CLK/DT are harmless in parallel.
- If an encoder turns backwards, swap its CLK/DT wires (or swap the two
  `gpios` entries for that knob in the overlay).

## Install

On the Pi:

```sh
cd ~/norns/rpi5
dtc -@ -I dts -O dtb -o norns-ky040.dtbo norns-ky040-overlay.dts
sudo cp norns-ky040.dtbo /boot/firmware/overlays/
echo 'dtoverlay=norns-ky040' | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

After boot you should have:

```
/dev/input/by-path/platform-keys-event     keys, EV_KEY codes 1/2/3
/dev/input/by-path/platform-knob1-event    EV_REL, +/-1 per detent
/dev/input/by-path/platform-knob2-event
/dev/input/by-path/platform-knob3-event
```

which is what `matronrc.lua` points matron at. To test without matron:
`evtest /dev/input/by-path/platform-knob1-event` and turn the knob.

## System notes (Pi 5 / Debian trixie)

- jackd runs as user `we` from a system unit, so `systemd-logind` must not
  reap its `/dev/shm` IPC when SSH sessions end — otherwise matron can
  never reconnect to a long-running jackd. Fixed with
  `/etc/systemd/logind.conf.d/norns-no-removeipc.conf`:

  ```ini
  [Login]
  RemoveIPC=no
  ```
