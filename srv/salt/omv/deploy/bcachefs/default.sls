bcachefs_tools_install:
  pkg.installed:
    - name: bcachefs-tools
    - refresh: True

{% if salt['cmd.retcode']('modinfo bcachefs', python_shell=False) != 0 %}
bcachefs_kernel_dkms_install:
  pkg.installed:
    - name: bcachefs-kernel-dkms
    - require:
      - pkg: bcachefs_tools_install

bcachefs_kmod_load:
  cmd.run:
    - name: modprobe --quiet bcachefs
    - unless: lsmod | grep -q '^bcachefs '
    - require:
      - pkg: bcachefs_kernel_dkms_install
{% else %}
bcachefs_kmod_load:
  cmd.run:
    - name: modprobe --quiet bcachefs
    - unless: lsmod | grep -q '^bcachefs '
    - require:
      - pkg: bcachefs_tools_install
{% endif %}
