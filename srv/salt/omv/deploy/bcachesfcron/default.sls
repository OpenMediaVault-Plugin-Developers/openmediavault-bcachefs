{% set snapshot_jobs = salt['omv_conf.get_by_filter'](
  'conf.service.bcachefs.snapshotjob',
  {'operator': 'stringNotEquals', 'arg0': 'uuid', 'arg1': ''}) | default([]) %}

{% set scrub_jobs = salt['omv_conf.get_by_filter'](
  'conf.service.bcachefs.scrubjob',
  {'operator': 'stringNotEquals', 'arg0': 'uuid', 'arg1': ''}) | default([]) %}

configure_bcachefs_snapshot_cron:
  file.managed:
    - name: "/etc/cron.d/openmediavault-bcachefs-snapshots"
    - source:
      - salt://{{ tpldir }}/files/etc_cron.d_omv-bcachefs-snapshots.j2
    - template: jinja
    - context:
        jobs: {{ snapshot_jobs | json }}
    - user: root
    - group: root
    - mode: 644

configure_bcachefs_scrub_cron:
  file.managed:
    - name: "/etc/cron.d/openmediavault-bcachefs-scrub"
    - source:
      - salt://{{ tpldir }}/files/etc_cron.d_omv-bcachefs-scrub.j2
    - template: jinja
    - context:
        jobs: {{ scrub_jobs | json }}
    - user: root
    - group: root
    - mode: 644
