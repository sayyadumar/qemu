rx_ss = ss.source_set()
rx_ss.add(when: 'CONFIG_RX_GDBSIM', if_true: files('rx-gdbsim.c'))
rx_ss.add(when: 'CONFIG_RX62N_MCU', if_true: files('rx62n.c'))
rx_ss.add(when: 'CONFIG_RX65N_EVK', if_true: files('rx65n-evk.c'))
rx_ss.add(when: 'CONFIG_RX65N_MCU', if_true: files('rx65n.c'))
rx_ss.add(when: 'CONFIG_RX72M_RSK', if_true: files('rx72m-rsk.c'))
rx_ss.add(when: 'CONFIG_RX72M_MCU', if_true: files('rx72m.c'))

hw_arch += {'rx': rx_ss}
