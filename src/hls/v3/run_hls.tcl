# vitis_hls -f run_hls.tcl [csim|synth|cosim|export]  (args via env STEP)
set step [expr {[info exists ::env(STEP)] ? $::env(STEP) : "csim"}]
set nm [expr {[info exists ::env(NMUX)] ? $::env(NMUX) : 2}]
open_project -reset dvbt3_prj$nm
set_top dvbt_mod
set nmux [expr {[info exists ::env(NMUX)] ? $::env(NMUX) : 2}]
add_files dvbt_mod.cpp -cflags "-DDVBT_NMUX=$nmux"
add_files -tb tb.cpp -cflags "-DDVBT_NMUX=$nmux"
open_solution -reset sol1 -flow_target vivado
set_part xc7z010clg400-1
create_clock -period 10 -name default
set_clock_uncertainty 1.5
config_rtl -reset all
config_export -format ip_catalog -vendor fishball -library edgeqam -version 1.0 -ipname dvbt_mod3_n$nmux
if {$step eq "csim" || $step eq "all"} {
  csim_design -argv "[pwd]/in.ts [pwd]/ref$nmux.cf32 150"
}
if {$step eq "synth" || $step eq "all" || $step eq "export"} {
  csynth_design
}
if {$step eq "export" || $step eq "all"} {
  export_design -rtl verilog -format ip_catalog
}
exit
