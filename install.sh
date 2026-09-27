#!/bin/bash

source .venv/bin/activate
meson setup build
meson compile -C build
sudo meson install -C build
