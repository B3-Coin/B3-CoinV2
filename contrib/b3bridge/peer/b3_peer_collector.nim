# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
#
# Network initialization follows Nimbus nimbus_light_client.nim v26.10.1,
# copyright (c) 2022-2026 Status Research & Development GmbH, used under MIT.
# Nimbus source commit: 963756c91c19b7824cea57e7aab5857ce2c776c7.
#
# Availability adapter only. B3 independently checks every emitted proof.
# No execution RPC, engine, transaction, optimistic-header or sequencer API.

{.push raises: [], gcsafe.}

import
  std/os,
  chronicles, chronos,
  ./networking/network_metadata_downloads,
  ./spec/beaconstate,
  ./spec/eth2_apis/eth2_rest_serialization,
  ./[beacon_clock, light_client, nimbus_binary_common, process_state]

const
  B3NimbusCommit = "963756c91c19b7824cea57e7aab5857ce2c776c7"
  MaxProofLine = 262144

proc emitProof[T](kind: string, obj: T) =
  # The OS pipe supplies bounded backpressure. A blocked consumer cannot
  # authorize an anchor, and the owning launcher enforces a run deadline.
  withForkyObject(obj):
    when lcDataFork > LightClientDataFork.None:
      try:
        let line = "{\"kind\":\"" & kind & "\",\"data\":" &
          RestJson.encode(forkyObject) & "}\n"
        if line.len > MaxProofLine:
          writePanicLine "B3 peer proof exceeds the bounded output frame"
          quit QuitFailure
        stdout.write(line)
        stdout.flushFile()
      except CatchableError:
        writePanicLine "B3 peer proof output failed"
        quit QuitFailure

proc runCollector(config: LightClientConf) {.raises: [CatchableError].} =
  var config = config
  # Refuse using this tool as a generic beacon/engine launcher. Its network
  # and proof authority are independently pinned by the native B3 policy.
  if config.eth2Network.get("mainnet") != "mainnet" or
      config.elUrls.len != 0 or config.web3Urls.len != 0 or
      config.jwtSecret.isSome or config.configFile.isSome or
      config.directPeers.len != 0 or config.bootstrapNodes.len != 0 or
      string(config.bootstrapNodesFile).len != 0 or
      config.maxPeers < 1 or config.maxPeers > 16 or
      config.hardMaxPeers.get(config.maxPeers) > 24 or
      config.metricsEnabled:
    raise newException(ValueError, "B3 collector requires bounded mainnet peer-only configuration")

  let metadata = loadEth2Network(config.eth2Network)
  for node in metadata.bootstrapNodes:
    config.bootstrapNodes.add node
  template cfg(): auto = metadata.cfg
  let
    genesisState = waitFor metadata.fetchGenesisState()
    genesisTime = genesisState[].genesis_time
    beaconClock = BeaconClock.init(cfg.timeParams, genesisTime).valueOr:
      raise newException(ValueError, "Invalid Ethereum beacon clock")
    getBeaconTime = beaconClock.getBeaconTimeFn()
    genesisValidatorsRoot = genesisState[].genesis_validators_root
    forkDigests = newClone ForkDigests.init(cfg, genesisValidatorsRoot)
    genesisBlockRoot = get_initial_beacon_block(genesisState[]).root
    rng = HmacDrbgContext.new()
    # Ephemeral peer identity is never exported and carries no bridge power.
    network = createEth2Node(
      rng, config, getRandomNetKeys(rng), cfg, forkDigests,
      getBeaconTime, genesisValidatorsRoot).valueOr:
        raise newException(ValueError, "Cannot initialize Ethereum peer network")
    client = createLightClient(
      network, rng, config, cfg, forkDigests, getBeaconTime,
      genesisValidatorsRoot, LightClientFinalizationMode.Strict)

  client.bootstrapObserver = proc(
      unused: LightClient, obj: ForkedLightClientBootstrap) =
    emitProof("bootstrap", obj)
  client.updateObserver = proc(
      unused: LightClient, obj: ForkedLightClientUpdate) =
    emitProof("update", obj)
  client.finalityUpdateObserver = proc(
      unused: LightClient, obj: ForkedLightClientFinalityUpdate) =
    emitProof("finality_update", obj)
  # No optimistic callback and no force-update fallback in Strict mode.
  client.trustedBlockRoot = some config.trustedBlockRoot
  network.registerProtocol(PeerSync, PeerSync.NetworkState.init(
    cfg, forkDigests, genesisBlockRoot, getBeaconTime))
  client.installMessageValidators()
  waitFor network.startListening()
  waitFor network.start()
  client.start()

  proc onSecond() {.async.} =
    while true:
      client.updateGossipStatus(beaconClock.currentSlot + 1)
      await chronos.sleepAsync(chronos.seconds(1))
  asyncSpawn onSecond()
  while not ProcessState.stopIt(notice "B3 peer collector stopping"):
    poll()

proc main() {.raises: [CatchableError].} =
  if commandLineParams() == @["--b3-version"]:
    stdout.writeLine("{\"format\":\"b3-nimbus-peer-collector/1\",\"upstream_commit\":\"" &
      B3NimbusCommit & "\",\"authority\":\"availability-only\"}")
    return
  ProcessState.setupStopHandlers()
  let config = LightClientConf.loadWithBanners(
    "B3 Ethereum peer proof collector", "MIT", [specBanner],
    setupLogger = false).valueOr:
      writePanicLine "Invalid B3 peer collector arguments"
      quit QuitFailure
  # Compiled log sinks go to stderr; stdout contains only raw proof frames.
  setLogLevel(LogLevel.WARN)
  runCollector(config)

when isMainModule:
  try:
    main()
  except CatchableError:
    writePanicLine "B3 Ethereum peer collector refused or disconnected"
    quit QuitFailure
