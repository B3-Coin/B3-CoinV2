# B3 Hive release operations (update manifests)

Offline signing workflow (doc/design/b3-hive-update-system.md). Private
signing keys NEVER enter the source tree, build environment, installer or
update server; production keys live on offline operator machines only.

```
# build the tool (any machine)
cmake -B build -DB3_UPDATE_TOOLS=ON && cmake --build build --target b3hive-sign

# one-time, per release operator, OFFLINE
b3hive-sign genkey release-key-1.hex          # chmod 0600; keep offline
b3hive-sign pubkey release-key-1.hex          # -> pubkey + keyid for pinning

# per release: author the payload (byte-exact, LF-only, see the design doc),
# then each operator signs the SAME payload bytes independently:
b3hive-sign sign manifest.payload release-key-1.hex >> sigs.txt

# assemble + verify with the production verifier before publishing:
cat manifest.payload > manifest.txt
echo "-----SIGNATURES-----" >> manifest.txt
cat sigs.txt >> manifest.txt
b3hive-sign verify manifest.txt 2 <pub1hex>,<pub2hex>,<pub3hex>
```

Production client configuration is compiled into each Qt release. The public
inputs are comma-separated where noted:

```
cmake -B build-release \
  -DBUILD_GUI=ON \
  -DB3_REQUIRE_UPDATE_CHANNEL=ON \
  -DB3_UPDATE_MANIFEST_URL=https://<manifest-host>/<stable-manifest> \
  -DB3_UPDATE_PUBLIC_KEYS=<pub1hex>,<pub2hex>,<pub3hex> \
  -DB3_UPDATE_SIGNATURE_THRESHOLD=2 \
  -DB3_UPDATE_ALLOWED_HOSTS=<manifest-host>,<artifact-host>,<redirect-host>
```

`B3_REQUIRE_UPDATE_CHANNEL=ON` makes configuration fail if the URL, keys,
threshold, or allowed hosts are missing or malformed, so release automation
cannot silently publish another GUI with updates disabled. The manifest's
initial host and every HTTPS redirect/artifact host must be listed. Public
keys are validated again by the client as fully valid, distinct x-only keys.

The release workflow loads the public values tracked in
`stable-channel-v1.cmake` and passes the required-channel gate to every Qt
build. Keeping the trust inputs in the tagged source makes an official build
reproducible and prevents mutable runner or repository variables from silently
changing a tag's updater authorities. Rotate that tracked file in a reviewed
release; never add the corresponding private keys. Runtime `-hiveupdate*`
arguments remain available only in non-release developer builds and cannot
override production trust.

Current release packaging uses manifest format `targz` for the Linux GUI
`.tar.gz`, `zip` for macOS GUI archives, and `exe` for the Windows x86-64
installer. Automatic replacement remains unsupported: the client verifies and
downloads the signed artifact, then reports that installation is manual.

Unset production inputs keep ordinary local builds quietly fail-closed. A test
key must never ship as trusted production material.
