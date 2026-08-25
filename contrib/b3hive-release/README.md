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

Client configuration (release inputs — never invented, never defaulted):
`-hiveupdateurl`, repeated `-hiveupdatekey`, `-hiveupdatethreshold`,
optional `-hiveupdatehost`. Unset => the update system is quietly and
completely disabled (fail-closed). A test key must never ship as trusted
production material.
