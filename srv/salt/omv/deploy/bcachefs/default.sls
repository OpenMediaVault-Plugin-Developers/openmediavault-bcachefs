bcachefs_tools_install:
  pkg.installed:
    - name: bcachefs-tools
    - refresh: True

{% if not salt['environ.get']('DPKG_MAINTSCRIPT_PACKAGE', '') %}

{% set _kver = salt['cmd.run']('uname -r') %}
{% set _kmaj = _kver.split('.')[0] | int %}
{% set _kmin = _kver.split('.')[1].split('-')[0] | int %}
{% set _kernel_ok = (_kmaj > 6) or (_kmaj == 6 and _kmin >= 16) %}

{% if salt['cmd.retcode']('modinfo bcachefs', python_shell=False) != 0 and
      salt['cmd.retcode']('dpkg-query -W bcachefs-kernel-dkms', python_shell=False) != 0 and
      _kernel_ok %}
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

bcachefs_modules_load_conf:
  file.managed:
    - name: /etc/modules-load.d/bcachefs.conf
    - contents: "bcachefs"
    - user: root
    - group: root
    - mode: "0644"

{% endif %}
