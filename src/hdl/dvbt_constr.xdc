# dvbt_bridge: single-bit / gray-coded synchronisers between sys_cpu_clk and the AD9361 divided clock
set_false_path -to [get_cells -hier -filter {NAME =~ */dvbt_bridge/inst/en_d_m_reg[0]}]
set_false_path -to [get_cells -hier -filter {NAME =~ */dvbt_bridge/inst/started_m_reg[0]}]
set_false_path -to [get_cells -hier -filter {NAME =~ */dvbt_bridge/inst/unf_gray_m1_reg[*]}]
