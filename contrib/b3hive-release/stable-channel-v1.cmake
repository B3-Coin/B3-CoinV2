# B3 Hive stable updater trust, generation 1.
#
# This file contains public, auditable build inputs only. The corresponding
# private release-signing keys must remain offline and must never enter this
# repository or GitHub Actions. FORCE prevents stale cache values from
# silently replacing the tagged trust root. Official release automation must
# not append later command-line overrides for these variables.
set(B3_UPDATE_MANIFEST_URL
    "https://raw.githubusercontent.com/B3-Coin/B3-CoinV2/master/contrib/b3hive-release/stable-manifest.txt"
    CACHE STRING "B3 Hive stable update manifest" FORCE)
set(B3_UPDATE_PUBLIC_KEYS
    "a28cd03b4c2ae70ab6ce466b3ea39cd7987820bb71725de72aff29fd38020ec7,425c5edad3ae7e16188bacb77c4d0d525ce73f2ec7bf17456f9980aee9e91675,d106e770a46be58b0691f1786dabf06d14b103b89df896002c516b93592e4673"
    CACHE STRING "B3 Hive stable release public keys" FORCE)
set(B3_UPDATE_SIGNATURE_THRESHOLD "2"
    CACHE STRING "B3 Hive stable release signature threshold" FORCE)
set(B3_UPDATE_ALLOWED_HOSTS
    "raw.githubusercontent.com,github.com,release-assets.githubusercontent.com,objects.githubusercontent.com"
    CACHE STRING "B3 Hive stable update hosts" FORCE)
