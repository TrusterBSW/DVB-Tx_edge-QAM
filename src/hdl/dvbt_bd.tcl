# DVB-T edgeQAM modulator (project fishball_dvbt_iio), sourced at the end of pluto/system_bd.tcl.
# Inserts dvbt_bridge between axi_ad9361_dac_dma and util_ad9361_dac_upack, and in front of
# axi_ad9361_dac_fifo din_data_0/1; dvbt_mod (Vitis HLS, ip/edgeqam) runs on sys_cpu_clk.

set dvbt_here [file dirname [info script]]
set dvbt_ip [file normalize [file join $dvbt_here ../../ip/edgeqam]]
set_property ip_repo_paths [concat [get_property ip_repo_paths [current_project]] [list $dvbt_ip]] [current_project]
update_ip_catalog -rebuild
add_files -norecurse [file join $dvbt_here dvbt_bridge.v]
add_files -fileset constrs_1 -norecurse [file join $dvbt_here dvbt_constr.xdc]
update_compile_order -fileset sources_1

# v1: one multiplex (IP dvbt_mod); v2: set dvbt_nmux 2 in the project (IP dvbt_mod_n2)
if {![info exists dvbt_nmux]} { set dvbt_nmux 1 }
if {$dvbt_nmux == 1} {
  create_bd_cell -type ip -vlnv fishball:edgeqam:dvbt_mod:1.0 dvbt_mod
} else {
  # v2 IP: dvbt_mod_n<N> ; v3 (shared chain, 4 mux): set dvbt_ipname dvbt_mod3_n4 in the project
  if {![info exists dvbt_ipname]} { set dvbt_ipname dvbt_mod_n${dvbt_nmux} }
  create_bd_cell -type ip -vlnv fishball:edgeqam:${dvbt_ipname}:1.0 dvbt_mod
}
create_bd_cell -type module -reference dvbt_bridge dvbt_bridge

# nets that the bridge takes over
delete_bd_objs [get_bd_intf_nets -of_objects [get_bd_intf_pins axi_ad9361_dac_dma/m_axis]]
delete_bd_objs [get_bd_nets -of_objects [get_bd_pins axi_ad9361_dac_fifo/din_data_0]]
delete_bd_objs [get_bd_nets -of_objects [get_bd_pins axi_ad9361_dac_fifo/din_data_1]]
set dvbt_upack_valid_net [get_bd_nets -of_objects [get_bd_pins axi_ad9361_dac_fifo/din_valid_in_0]]
delete_bd_objs $dvbt_upack_valid_net

# clocks and resets
ad_connect sys_cpu_clk dvbt_bridge/s_clk
ad_connect sys_cpu_clk dvbt_mod/ap_clk
ad_connect sys_cpu_resetn dvbt_bridge/s_resetn
ad_connect dvbt_bridge/hls_rst_n dvbt_mod/ap_rst_n
ad_connect util_ad9361_divclk/clk_out dvbt_bridge/d_clk

# control / status through the DAC GPIO registers
ad_connect axi_ad9361/up_dac_gpio_out dvbt_bridge/gpio_out
ad_connect dvbt_bridge/gpio_in axi_ad9361/up_dac_gpio_in

# HLS core streams
ad_connect dvbt_bridge/ts_data dvbt_mod/ts_in_TDATA
ad_connect dvbt_bridge/ts_valid dvbt_mod/ts_in_TVALID
ad_connect dvbt_mod/ts_in_TREADY dvbt_bridge/ts_ready
ad_connect dvbt_mod/iq_out_TDATA dvbt_bridge/iq_data
ad_connect dvbt_mod/iq_out_TVALID dvbt_bridge/iq_valid
ad_connect dvbt_bridge/iq_ready dvbt_mod/iq_out_TREADY
ad_connect dvbt_mod/st_ts_TDATA dvbt_bridge/stts_data
ad_connect dvbt_mod/st_ts_TVALID dvbt_bridge/stts_valid
ad_connect dvbt_bridge/stts_ready dvbt_mod/st_ts_TREADY
ad_connect dvbt_mod/st_fft_TDATA dvbt_bridge/stfft_data
ad_connect dvbt_mod/st_fft_TVALID dvbt_bridge/stfft_valid
ad_connect dvbt_bridge/stfft_ready dvbt_mod/st_fft_TREADY
if {$dvbt_nmux > 1} {
  ad_connect dvbt_bridge/ctl_data dvbt_mod/ctl_TDATA
  ad_connect dvbt_bridge/ctl_valid dvbt_mod/ctl_TVALID
  ad_connect dvbt_mod/ctl_TREADY dvbt_bridge/ctl_ready
}

# TX DMA stream: to the modulator or, when disabled, to upack as before
ad_connect axi_ad9361_dac_dma/m_axis_data dvbt_bridge/dma_data
ad_connect axi_ad9361_dac_dma/m_axis_valid dvbt_bridge/dma_valid
ad_connect dvbt_bridge/dma_ready axi_ad9361_dac_dma/m_axis_ready
ad_connect dvbt_bridge/up_data util_ad9361_dac_upack/s_axis_data
ad_connect dvbt_bridge/up_valid util_ad9361_dac_upack/s_axis_valid
ad_connect util_ad9361_dac_upack/s_axis_ready dvbt_bridge/up_ready

# DAC FIFO input
ad_connect axi_ad9361_dac_fifo/din_valid_0 dvbt_bridge/dac_req
if {[info exists dvbt_no_fir]} {
  # dedicated design: no TX FIR, upack feeds the DAC FIFO; TX2 also through the bridge (0 in TS mode)
  ad_connect util_ad9361_dac_upack/fifo_rd_data_0 dvbt_bridge/in_d0
  ad_connect util_ad9361_dac_upack/fifo_rd_data_1 dvbt_bridge/in_d1
  delete_bd_objs [get_bd_nets -of_objects [get_bd_pins axi_ad9361_dac_fifo/din_data_2]]
  delete_bd_objs [get_bd_nets -of_objects [get_bd_pins axi_ad9361_dac_fifo/din_data_3]]
  ad_connect util_ad9361_dac_upack/fifo_rd_data_2 dvbt_bridge/in_d2
  ad_connect util_ad9361_dac_upack/fifo_rd_data_3 dvbt_bridge/in_d3
  ad_connect dvbt_bridge/dac_d2 axi_ad9361_dac_fifo/din_data_2
  ad_connect dvbt_bridge/dac_d3 axi_ad9361_dac_fifo/din_data_3
} else {
  ad_connect tx_fir_interpolator/data_out_0 dvbt_bridge/in_d0
  ad_connect tx_fir_interpolator/data_out_1 dvbt_bridge/in_d1
}
ad_connect util_ad9361_dac_upack/fifo_rd_valid dvbt_bridge/in_valid
ad_connect dvbt_bridge/dac_d0 axi_ad9361_dac_fifo/din_data_0
ad_connect dvbt_bridge/dac_d1 axi_ad9361_dac_fifo/din_data_1
for {set i 0} {$i < 4} {incr i} {
  ad_connect dvbt_bridge/dac_valid axi_ad9361_dac_fifo/din_valid_in_$i
}
