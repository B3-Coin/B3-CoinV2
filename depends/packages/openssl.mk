package=openssl
# OpenSSL 3.5 LTS. Checksum read from the official release asset:
# https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz.sha256
$(package)_version=3.5.8
$(package)_download_path=https://github.com/openssl/openssl/releases/download/openssl-$($(package)_version)
$(package)_file_name=openssl-$($(package)_version).tar.gz
$(package)_sha256_hash=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2

define $(package)_set_vars
  $(package)_config_opts=--prefix=$(host_prefix) --libdir=lib --openssldir=/etc/ssl
  $(package)_config_opts+=no-shared no-module no-tests no-apps no-docs no-legacy no-engine no-comp
  $(package)_config_opts+=no-ssl3 no-tls1 no-tls1_1
  $(package)_config_opts_x86_64_linux=linux-x86_64
  $(package)_config_opts_aarch64_linux=linux-aarch64
  $(package)_config_opts_arm_linux=linux-armv4
  $(package)_config_opts_i686_linux=linux-x86
  $(package)_config_opts_x86_64_darwin=darwin64-x86_64-cc
  $(package)_config_opts_aarch64_darwin=darwin64-arm64-cc
  $(package)_config_opts_x86_64_mingw32=mingw64
  $(package)_config_opts_i686_mingw32=mingw
  $(package)_config_opts_x86_64_freebsd=BSD-x86_64
  $(package)_config_opts_x86_64_netbsd=BSD-x86_64
  $(package)_config_opts_x86_64_openbsd=BSD-x86_64
  $(package)_config_opts_riscv64_linux=linux64-riscv64
  $(package)_cflags+=-fPIC -fdebug-prefix-map=$($(package)_extract_dir)=/usr -fmacro-prefix-map=$($(package)_extract_dir)=/usr
  $(package)_config_env+=CC="$($(package)_cc)" AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)"
  $(package)_config_env+=CFLAGS="$($(package)_cflags)" CPPFLAGS="$($(package)_cppflags)" LDFLAGS="$($(package)_ldflags)"
endef

define $(package)_config_cmds
  ./Configure $($(package)_config_opts)
endef

define $(package)_build_cmds
  $(MAKE) build_libs
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_sw
endef
