bcachefs_tools_install:
  pkg.installed:
    - name: bcachefs-tools
    - refresh: True

{% if not salt['environ.get']('DPKG_MAINTSCRIPT_PACKAGE', '') %}

{% set config = salt['omv_conf.get']('conf.service.bcachefs') %}
{% set channel = 'snapshot' if config.suite == 'bcachefs-tools-snapshot' else 'release' %}

# bcachefs is no longer maintained in mainline, so we never use an in-tree
# module. Try to install a prebuilt module matching the running kernel from
# module.bcachefs.org (Debian amd64/arm64 kernels; .ko or .ko.xz; no local
# build). The helper exits non-zero when no matching module exists, so
# "|| true" keeps the state green and lets the DKMS fallback below take over.
bcachefs_module_prebuilt:
  cmd.run:
    - name: omv-bcachefs-module install || true
    - env:
      - BCACHEFS_MODULE_CHANNEL: {{ channel }}
    - require:
      - pkg: bcachefs_tools_install

# Fallback for the Proxmox kernel and any kernel without a prebuilt module:
# build it locally with DKMS. Needs the matching kernel headers only -- the
# bcachefs kernel module builds as C, so no Rust toolchain is required (the
# Rust in bcachefs-tools is a separate prebuilt package).
bcachefs_kernel_dkms_install:
  pkg.installed:
    - name: bcachefs-kernel-dkms
    - unless: 'test -f /lib/modules/$(uname -r)/updates/bcachefs.ko'
    - require:
      - cmd: bcachefs_module_prebuilt

bcachefs_kmod_load:
  cmd.run:
    - name: modprobe --quiet bcachefs || true
    - unless: lsmod | grep -q '^bcachefs '
    - require:
      - cmd: bcachefs_module_prebuilt
      - pkg: bcachefs_kernel_dkms_install

# Generic build verification: confirm a usable module actually exists for the
# running kernel after provisioning. Fails loudly (visible in Apply Changes)
# instead of silently leaving a broken state if the prebuilt fetch and the DKMS
# build both produced nothing. The bcachefs module builds as C, so a missing
# Rust toolchain is not what breaks a DKMS build here.
bcachefs_module_verify:
  cmd.run:
    - name: |
        if modinfo -k "$(uname -r)" bcachefs >/dev/null 2>&1; then
          echo "bcachefs: kernel module present for $(uname -r)"
        else
          echo "bcachefs: ERROR - no kernel module available for $(uname -r) after provisioning." >&2
          echo "If DKMS was used, inspect the build log under /var/lib/dkms/bcachefs/*/build/make.log" >&2
          exit 1
        fi
    - require:
      - cmd: bcachefs_kmod_load

bcachefs_modules_load_conf:
  file.managed:
    - name: /etc/modules-load.d/bcachefs.conf
    - contents: "bcachefs"
    - user: root
    - group: root
    - mode: '0644'

{% endif %}
