#!/bin/bash
# Test of the encryption of the face models (AES-256-GCM, key from a systemd
# credential). No camera, bus or TPM needed. See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
CXXFLAGS+=(-I"$SRC_DIR")

link "$WORK_DIR/model_crypto_test" "${BASE_SOURCES[@]}" "$SRC_DIR/management/model_store.cc" "$TEST_DIR/model_crypto_test.cc"
"$WORK_DIR/model_crypto_test" "$T/model_crypto"
