bcachefs_tools_install:
  pkg.installed:
    - name: bcachefs-tools
    - refresh: True

{% if not salt['environ.get']('DPKG_MAINTSCRIPT_PACKAGE', '') %}

{% if salt['cmd.retcode']('modinfo bcachefs', python_shell=False) != 0 and
      salt['cmd.retcode']('dpkg-query -W bcachefs-kernel-dkms', python_shell=False) != 0 %}
bcachefs_kernel_dkms_install:
  pkg.installed:
    - name: bcachefs-kernel-dkms
    - require:
      - pkg: bcachefs_tools_install
{% endif %}

bcachefs_kmod_load:
  cmd.run:
    - name: modprobe --quiet bcachefs || true
    - unless: lsmod | grep -q '^bcachefs '
    - require:
      - pkg: bcachefs_tools_install

{% endif %}
