#!/bin/bash

source .venv/bin/activate
meson compile -C build
sudo meson install -C build
