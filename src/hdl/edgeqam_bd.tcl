# Dedicated edgeQAM design for the Fishball 7010 (project fishball_edgeqam):
# PS7 + axi_ad9361 + RX DMA (needed by the Linux AD9361 driver) + TX DMA -> dvbt_bridge /
# dvbt_mod -> DAC. No Maia, no FIR, no web UI logic. Same addresses and interrupts as Tezuka,
# so the Tezuka devicetree is used unchanged (the maia-sdr UIO node stays, unused).
#
# The PS7 / DDR / MIO / ports part is taken unchanged from pluto/system_bd.tcl (everything
# before "# ad9361 core(s)") with fishball and LVDS_ENABLE set by the project.

set edgeqam_here [file dirname [info script]]
set f [open [file join $edgeqam_here system_bd.tcl]]
set t [read $f]
close $f
set i [string first "# ad9361 core(s)" $t]
set g [open [file join $edgeqam_here edgeqam_head_gen.tcl] w]
puts $g [string range $t 0 [expr {$i - 1}]]
close $g
source [file join $edgeqam_here edgeqam_head_gen.tcl]

# no Maia: HP1 (spectrometer) unused
ad_ip_parameter sys_ps7 CONFIG.PCW_USE_S_AXI_HP1 0
ad_connect gpio_o sys_ps7/GPIO_O

# ---------------------------------------------------------------- AD9361 core (as Tezuka)
ad_ip_instance axi_ad9361 axi_ad9361
ad_ip_parameter axi_ad9361 CONFIG.ID 0
ad_ip_parameter axi_ad9361 CONFIG.CMOS_OR_LVDS_N 0
ad_ip_parameter axi_ad9361 CONFIG.MODE_1R1T 0
ad_ip_parameter axi_ad9361 CONFIG.ADC_INIT_DELAY 30
ad_ip_parameter axi_ad9361 CONFIG.TDD_DISABLE 1
ad_ip_parameter axi_ad9361 CONFIG.DAC_DDS_DISABLE 1

ad_connect rx_clk_in_p axi_ad9361/rx_clk_in_p
ad_connect rx_clk_in_n axi_ad9361/rx_clk_in_n
ad_connect rx_frame_in_p axi_ad9361/rx_frame_in_p
ad_connect rx_frame_in_n axi_ad9361/rx_frame_in_n
ad_connect rx_data_in_p axi_ad9361/rx_data_in_p
ad_connect rx_data_in_n axi_ad9361/rx_data_in_n
ad_connect tx_clk_out_p axi_ad9361/tx_clk_out_p
ad_connect tx_clk_out_n axi_ad9361/tx_clk_out_n
ad_connect tx_frame_out_p axi_ad9361/tx_frame_out_p
ad_connect tx_frame_out_n axi_ad9361/tx_frame_out_n
ad_connect tx_data_out_p axi_ad9361/tx_data_out_p
ad_connect tx_data_out_n axi_ad9361/tx_data_out_n
ad_connect enable axi_ad9361/enable
ad_connect txnrx axi_ad9361/txnrx
ad_connect up_enable axi_ad9361/up_enable
ad_connect up_txnrx axi_ad9361/up_txnrx
ad_connect axi_ad9361/tdd_sync GND
ad_connect sys_200m_clk axi_ad9361/delay_clk
ad_connect axi_ad9361/l_clk axi_ad9361/clk

# ---------------------------------------------------------------- sample clock (l_clk / 4 or 2)
ad_ip_instance xlconcat util_ad9361_divclk_sel_concat
ad_ip_parameter util_ad9361_divclk_sel_concat CONFIG.NUM_PORTS 2
ad_connect axi_ad9361/adc_r1_mode util_ad9361_divclk_sel_concat/In0
ad_connect axi_ad9361/dac_r1_mode util_ad9361_divclk_sel_concat/In1
ad_ip_instance util_reduced_logic util_ad9361_divclk_sel
ad_ip_parameter util_ad9361_divclk_sel CONFIG.C_SIZE 2
ad_connect util_ad9361_divclk_sel_concat/dout util_ad9361_divclk_sel/Op1
ad_ip_instance util_clkdiv util_ad9361_divclk
ad_ip_parameter util_ad9361_divclk CONFIG.SEL_0_DIV 4
ad_ip_parameter util_ad9361_divclk CONFIG.SEL_1_DIV 2
ad_connect util_ad9361_divclk_sel/Res util_ad9361_divclk/clk_sel
ad_connect axi_ad9361/l_clk util_ad9361_divclk/clk
ad_ip_instance proc_sys_reset util_ad9361_divclk_reset
ad_connect sys_rstgen/peripheral_aresetn util_ad9361_divclk_reset/ext_reset_in
ad_connect util_ad9361_divclk/clk_out util_ad9361_divclk_reset/slowest_sync_clk

# ---------------------------------------------------------------- RX: fifo -> cpack -> DMA
ad_ip_instance util_wfifo util_ad9361_adc_fifo
ad_ip_parameter util_ad9361_adc_fifo CONFIG.NUM_OF_CHANNELS 4
ad_ip_parameter util_ad9361_adc_fifo CONFIG.DIN_ADDRESS_WIDTH 4
ad_ip_parameter util_ad9361_adc_fifo CONFIG.DIN_DATA_WIDTH 16
ad_ip_parameter util_ad9361_adc_fifo CONFIG.DOUT_DATA_WIDTH 16
ad_connect axi_ad9361/l_clk util_ad9361_adc_fifo/din_clk
ad_connect axi_ad9361/rst util_ad9361_adc_fifo/din_rst
ad_connect util_ad9361_divclk/clk_out util_ad9361_adc_fifo/dout_clk
ad_connect util_ad9361_divclk_reset/peripheral_aresetn util_ad9361_adc_fifo/dout_rstn

ad_ip_instance util_cpack2 util_ad9361_adc_pack { NUM_OF_CHANNELS 4 SAMPLE_DATA_WIDTH 16 }
ad_connect util_ad9361_divclk/clk_out util_ad9361_adc_pack/clk
ad_connect util_ad9361_divclk_reset/peripheral_reset util_ad9361_adc_pack/reset
ad_connect util_ad9361_adc_fifo/dout_valid_0 util_ad9361_adc_pack/fifo_wr_en
ad_connect util_ad9361_adc_pack/fifo_wr_overflow util_ad9361_adc_fifo/dout_ovf

set adc_ch {i0 q0 i1 q1}
for {set i 0} {$i < 4} {incr i} {
  set c [lindex $adc_ch $i]
  ad_connect axi_ad9361/adc_enable_$c util_ad9361_adc_fifo/din_enable_$i
  ad_connect axi_ad9361/adc_valid_$c util_ad9361_adc_fifo/din_valid_$i
  ad_connect axi_ad9361/adc_data_$c util_ad9361_adc_fifo/din_data_$i
  ad_connect util_ad9361_adc_fifo/dout_enable_$i util_ad9361_adc_pack/enable_$i
  ad_connect util_ad9361_adc_fifo/dout_data_$i util_ad9361_adc_pack/fifo_wr_data_$i
}

ad_ip_instance axi_dmac axi_ad9361_adc_dma
ad_ip_parameter axi_ad9361_adc_dma CONFIG.DMA_TYPE_SRC 2
ad_ip_parameter axi_ad9361_adc_dma CONFIG.DMA_TYPE_DEST 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.CYCLIC 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.SYNC_TRANSFER_START 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.AXI_SLICE_SRC 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.AXI_SLICE_DEST 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.DMA_2D_TRANSFER 0
ad_ip_parameter axi_ad9361_adc_dma CONFIG.DMA_DATA_WIDTH_SRC 64
ad_connect util_ad9361_divclk/clk_out axi_ad9361_adc_dma/fifo_wr_clk
ad_connect axi_ad9361_adc_dma/fifo_wr util_ad9361_adc_pack/packed_fifo_wr
ad_connect sys_cpu_resetn axi_ad9361_adc_dma/m_dest_axi_aresetn

# ---------------------------------------------------------------- TX: DMA -> upack -> fifo -> DAC
ad_ip_instance axi_dmac axi_ad9361_dac_dma
ad_ip_parameter axi_ad9361_dac_dma CONFIG.DMA_TYPE_SRC 0
ad_ip_parameter axi_ad9361_dac_dma CONFIG.DMA_TYPE_DEST 1
ad_ip_parameter axi_ad9361_dac_dma CONFIG.CYCLIC 1
ad_ip_parameter axi_ad9361_dac_dma CONFIG.AXI_SLICE_SRC 0
ad_ip_parameter axi_ad9361_dac_dma CONFIG.AXI_SLICE_DEST 0
ad_ip_parameter axi_ad9361_dac_dma CONFIG.DMA_2D_TRANSFER 0
ad_ip_parameter axi_ad9361_dac_dma CONFIG.DMA_DATA_WIDTH_DEST 64
ad_connect util_ad9361_divclk/clk_out axi_ad9361_dac_dma/m_axis_aclk
ad_connect sys_cpu_resetn axi_ad9361_dac_dma/m_src_axi_aresetn

ad_ip_instance util_upack2 util_ad9361_dac_upack { NUM_OF_CHANNELS 4 SAMPLE_DATA_WIDTH 16 }
ad_connect util_ad9361_divclk/clk_out util_ad9361_dac_upack/clk
ad_connect util_ad9361_divclk_reset/peripheral_reset util_ad9361_dac_upack/reset
ad_connect axi_ad9361_dac_dma/m_axis util_ad9361_dac_upack/s_axis

ad_ip_instance util_rfifo axi_ad9361_dac_fifo
ad_ip_parameter axi_ad9361_dac_fifo CONFIG.DIN_DATA_WIDTH 16
ad_ip_parameter axi_ad9361_dac_fifo CONFIG.DOUT_DATA_WIDTH 16
ad_ip_parameter axi_ad9361_dac_fifo CONFIG.DIN_ADDRESS_WIDTH 4
ad_connect axi_ad9361/l_clk axi_ad9361_dac_fifo/dout_clk
ad_connect axi_ad9361/rst axi_ad9361_dac_fifo/dout_rst
ad_connect util_ad9361_divclk/clk_out axi_ad9361_dac_fifo/din_clk
ad_connect util_ad9361_divclk_reset/peripheral_aresetn axi_ad9361_dac_fifo/din_rstn
ad_connect axi_ad9361_dac_fifo/dout_unf axi_ad9361/dac_dunf
ad_connect util_ad9361_dac_upack/fifo_rd_underflow axi_ad9361_dac_fifo/din_unf
ad_connect axi_ad9361_dac_fifo/din_valid_0 util_ad9361_dac_upack/fifo_rd_en

set dac_ch {i0 q0 i1 q1}
for {set i 0} {$i < 4} {incr i} {
  set c [lindex $dac_ch $i]
  ad_connect axi_ad9361_dac_fifo/dout_enable_$i axi_ad9361/dac_enable_$c
  ad_connect axi_ad9361_dac_fifo/dout_valid_$i axi_ad9361/dac_valid_$c
  ad_connect axi_ad9361_dac_fifo/dout_data_$i axi_ad9361/dac_data_$c
  ad_connect axi_ad9361_dac_fifo/din_enable_$i util_ad9361_dac_upack/enable_$i
  ad_connect util_ad9361_dac_upack/fifo_rd_data_$i axi_ad9361_dac_fifo/din_data_$i
  ad_connect util_ad9361_dac_upack/fifo_rd_valid axi_ad9361_dac_fifo/din_valid_in_$i
}

# ---------------------------------------------------------------- CPU interconnect, memory, irq
ad_cpu_interconnect 0x79020000 axi_ad9361
ad_cpu_interconnect 0x7C400000 axi_ad9361_adc_dma
ad_cpu_interconnect 0x7C420000 axi_ad9361_dac_dma
ad_mem_hp2_interconnect sys_cpu_clk sys_ps7/S_AXI_HP2
ad_mem_hp2_interconnect sys_cpu_clk axi_ad9361_adc_dma/m_dest_axi
ad_mem_hp2_interconnect sys_cpu_clk axi_ad9361_dac_dma/m_src_axi
ad_cpu_interrupt ps-13 mb-13 axi_ad9361_adc_dma/irq
ad_cpu_interrupt ps-12 mb-12 axi_ad9361_dac_dma/irq

# ---------------------------------------------------------------- DVB-T modulator
# dvbt_bd.tcl reroutes the TX stream (FIR outputs replaced by upack outputs in this design)
set dvbt_no_fir 1
source [file join $edgeqam_here dvbt_bd.tcl]
