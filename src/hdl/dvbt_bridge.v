// dvbt_bridge: glue between the Fishball TX path and the dvbt_mod HLS core (edgeqam PoC).
//
// d_clk domain (util_ad9361_divclk, one sample per request):
//   axi_ad9361_dac_dma m_axis --> [en=0] util_ad9361_dac_upack s_axis (unchanged Tezuka path)
//                             --> [en=1] TS FIFO --> dvbt_mod (s_clk)
//   axi_ad9361_dac_fifo din   <-- [en=0] FIR / upack outputs (unchanged)
//                             <-- [en=1] IQ FIFO <-- dvbt_mod
// s_clk domain (sys_cpu_clk 100 MHz): dvbt_mod, control/status through the DAC GPIO registers
//   gpio_out (0x790240BC): bit 0 TX FIR (Tezuka, untouched), bit 1 DVB-T enable,
//                          bits 3:2 status select for gpio_in,
//                          bits 11:8 mute multiplex 0..3 (v2 bitstreams, ctl stream of dvbt_mod)
//   gpio_in  (0x790240B8): sel 0 OFDM symbols produced, 1 null packets inserted,
//                          2 {started, DAC underflow samples[15:0]},
//                          3 {sync errors + words dropped to resync [15:0], IFFT overflows[15:0]}
`timescale 1ns/1ps

module dvbt_bridge (
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_clk CLK" *)
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_RESET s_resetn" *)
  input             s_clk,
  input             s_resetn,
  input      [31:0] gpio_out,
  output reg [31:0] gpio_in,

  output reg        hls_rst_n,
  output     [31:0] ts_data,
  output            ts_valid,
  input             ts_ready,
  input      [31:0] iq_data,
  input             iq_valid,
  output            iq_ready,
  input      [15:0] stts_data,
  input             stts_valid,
  output            stts_ready,
  input      [15:0] stfft_data,
  input             stfft_valid,
  output            stfft_ready,
  output     [7:0]  ctl_data,      // v2 only (multiplex mask), leave unconnected for v1
  output reg        ctl_valid,
  input             ctl_ready,

  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 d_clk CLK" *)
  input             d_clk,
  input      [63:0] dma_data,
  input             dma_valid,
  output            dma_ready,
  output     [63:0] up_data,
  output            up_valid,
  input             up_ready,

  input             dac_req,
  input      [15:0] in_d0,
  input      [15:0] in_d1,
  input      [15:0] in_d2,
  input      [15:0] in_d3,
  input             in_valid,
  output reg [15:0] dac_d0,
  output reg [15:0] dac_d1,
  output reg [15:0] dac_d2,
  output reg [15:0] dac_d3,
  output reg        dac_valid
);

  // ---------------------------------------------------------------- s_clk control
  reg en_s = 1'b0;
  always @(posedge s_clk) begin
    en_s <= s_resetn & gpio_out[1];
    hls_rst_n <= s_resetn & en_s;
  end

  (* ASYNC_REG = "TRUE" *) reg [1:0] en_d_m = 2'b0;
  wire en_d = en_d_m[1];
  always @(posedge d_clk) en_d_m <= {en_d_m[0], en_s};

  // ---------------------------------------------------------------- TS FIFO d_clk -> s_clk
  wire        tsf_full, tsf_empty, tsf_wr_busy, tsf_rd_busy;
  wire [63:0] tsf_dout;
  wire        tsf_wr = en_d & dma_valid & ~tsf_full & ~tsf_wr_busy;
  reg         half = 1'b0;
  // packet resynchronisation: a packet must start with a word whose first byte is 0x47
  // (packets are 47 words, the DMA keeps the 32-bit alignment); otherwise the word is
  // dropped until a sync byte is found again (after a reset in the middle of the stream)
  reg  [5:0]  wc = 6'd0;
  wire        ts_avail;
  wire        hunt_drop;
  wire        ts_take = (ts_valid & ts_ready) | hunt_drop;
  wire        tsf_rd = ts_take & half;

  xpm_fifo_async #(
    .FIFO_MEMORY_TYPE("block"), .FIFO_WRITE_DEPTH(512), .WRITE_DATA_WIDTH(64),
    .READ_DATA_WIDTH(64), .READ_MODE("fwft"), .FIFO_READ_LATENCY(0),
    .CDC_SYNC_STAGES(2), .RELATED_CLOCKS(0), .USE_ADV_FEATURES("0000"),
    .ECC_MODE("no_ecc"), .FULL_RESET_VALUE(1), .DOUT_RESET_VALUE("0"),
    .WAKEUP_TIME(0), .SIM_ASSERT_CHK(0)
  ) i_ts_fifo (
    .rst(~en_d), .wr_clk(d_clk), .wr_en(tsf_wr), .din(dma_data), .full(tsf_full),
    .wr_rst_busy(tsf_wr_busy), .rd_clk(s_clk), .rd_en(tsf_rd), .dout(tsf_dout),
    .empty(tsf_empty), .rd_rst_busy(tsf_rd_busy),
    .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
    .overflow(), .prog_full(), .wr_data_count(), .almost_full(), .wr_ack(),
    .underflow(), .prog_empty(), .rd_data_count(), .almost_empty(), .data_valid(),
    .sbiterr(), .dbiterr()
  );

  // 64 -> 32 bits, low half first (first byte in memory = bits 7:0)
  assign ts_avail = ~tsf_empty & ~tsf_rd_busy & hls_rst_n;
  assign ts_data = half ? tsf_dout[63:32] : tsf_dout[31:0];
  assign hunt_drop = ts_avail & (wc == 6'd0) & (ts_data[7:0] != 8'h47);
  assign ts_valid = ts_avail & ~hunt_drop;
  reg [15:0] nresync = 16'd0;
  always @(posedge s_clk)
    if (!hls_rst_n) begin
      half <= 1'b0;
      wc <= 6'd0;
      nresync <= 16'd0;
    end else begin
      if (ts_take)
        half <= ~half;
      if (ts_valid & ts_ready)
        wc <= (wc == 6'd46) ? 6'd0 : wc + 1'b1;
      if (hunt_drop)
        nresync <= nresync + 1'b1;
    end

  assign dma_ready = en_d ? (~tsf_full & ~tsf_wr_busy) : up_ready;
  assign up_valid = en_d ? 1'b0 : dma_valid;
  assign up_data = dma_data;

  // ---------------------------------------------------------------- IQ FIFO s_clk -> d_clk
  wire        iqf_full, iqf_empty, iqf_wr_busy, iqf_rd_busy;
  wire [31:0] iqf_dout;
  wire [11:0] iqf_rd_count;
  reg         started = 1'b0;
  wire        iqf_rd = en_d & started & dac_req & ~iqf_empty & ~iqf_rd_busy;
  assign iq_ready = hls_rst_n & ~iqf_full & ~iqf_wr_busy;

  xpm_fifo_async #(
    .FIFO_MEMORY_TYPE("block"), .FIFO_WRITE_DEPTH(2048), .WRITE_DATA_WIDTH(32),
    .READ_DATA_WIDTH(32), .READ_MODE("fwft"), .FIFO_READ_LATENCY(0),
    .CDC_SYNC_STAGES(2), .RELATED_CLOCKS(0), .USE_ADV_FEATURES("0400"),
    .RD_DATA_COUNT_WIDTH(12), .ECC_MODE("no_ecc"), .FULL_RESET_VALUE(1),
    .DOUT_RESET_VALUE("0"), .WAKEUP_TIME(0), .SIM_ASSERT_CHK(0)
  ) i_iq_fifo (
    .rst(~hls_rst_n), .wr_clk(s_clk), .wr_en(iq_valid & iq_ready), .din(iq_data),
    .full(iqf_full), .wr_rst_busy(iqf_wr_busy), .rd_clk(d_clk), .rd_en(iqf_rd),
    .dout(iqf_dout), .empty(iqf_empty), .rd_rst_busy(iqf_rd_busy),
    .rd_data_count(iqf_rd_count),
    .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
    .overflow(), .prog_full(), .wr_data_count(), .almost_full(), .wr_ack(),
    .underflow(), .prog_empty(), .almost_empty(), .data_valid(),
    .sbiterr(), .dbiterr()
  );

  // DAC side: wait for 1024 samples, then one sample per request (zero on underflow)
  reg [15:0] unf_cnt = 16'd0;
  always @(posedge d_clk) begin
    if (!en_d) begin
      started <= 1'b0;
      unf_cnt <= 16'd0;
    end else begin
      if (iqf_rd_count >= 12'd1024 && !iqf_rd_busy)
        started <= 1'b1;
      if (started & dac_req & (iqf_empty | iqf_rd_busy))
        unf_cnt <= unf_cnt + 1'b1;
    end
    if (en_d) begin
      dac_d0 <= iqf_rd ? iqf_dout[15:0] : 16'd0;
      dac_d1 <= iqf_rd ? iqf_dout[31:16] : 16'd0;
      dac_d2 <= 16'd0;
      dac_d3 <= 16'd0;
      dac_valid <= dac_req;
    end else begin
      dac_d0 <= in_d0;
      dac_d1 <= in_d1;
      dac_d2 <= in_d2;
      dac_d3 <= in_d3;
      dac_valid <= in_valid;
    end
  end

  // underflow counter to s_clk (gray code)
  reg [15:0] unf_gray = 16'd0;
  always @(posedge d_clk) unf_gray <= unf_cnt ^ (unf_cnt >> 1);
  (* ASYNC_REG = "TRUE" *) reg [15:0] unf_gray_m1 = 16'd0, unf_gray_m2 = 16'd0;
  (* ASYNC_REG = "TRUE" *) reg [1:0] started_m = 2'b0;
  reg [15:0] unf_s;
  integer i;
  always @(posedge s_clk) begin
    unf_gray_m1 <= unf_gray;
    unf_gray_m2 <= unf_gray_m1;
    started_m <= {started_m[0], started};
    unf_s[15] = unf_gray_m2[15];
    for (i = 14; i >= 0; i = i - 1)
      unf_s[i] = unf_s[i + 1] ^ unf_gray_m2[i];
  end

  // ---------------------------------------------------------------- multiplex mask (s_clk)
  reg [3:0] mute_d = 4'hF;
  assign ctl_data = {4'hF, ~gpio_out[11:8]};
  always @(posedge s_clk)
    if (!hls_rst_n) begin
      mute_d <= 4'hF;          // forces one update after reset
      ctl_valid <= 1'b0;
    end else begin
      if (ctl_valid & ctl_ready)
        ctl_valid <= 1'b0;
      else if (gpio_out[11:8] != mute_d)
        ctl_valid <= 1'b1;
      if (!ctl_valid || ctl_ready)
        mute_d <= gpio_out[11:8];
    end

  // ---------------------------------------------------------------- status (s_clk)
  assign stts_ready = 1'b1;
  assign stfft_ready = 1'b1;
  reg [31:0] nsym = 32'd0, nnull = 32'd0;
  reg [15:0] nsync = 16'd0, novf = 16'd0;
  always @(posedge s_clk) begin
    if (!hls_rst_n) begin
      nsym <= 32'd0;
      nnull <= 32'd0;
      nsync <= 16'd0;
      novf <= 16'd0;
    end else begin
      if (stts_valid) begin
        nsym <= nsym + 1'b1;
        nnull <= nnull + stts_data[7:0];
        nsync <= nsync + stts_data[15:8];
      end
      if (stfft_valid && stfft_data[0])
        novf <= novf + 1'b1;
    end
    case (gpio_out[3:2])
      2'd0: gpio_in <= nsym;
      2'd1: gpio_in <= nnull;
      2'd2: gpio_in <= {started_m[1], 15'd0, unf_s};
      default: gpio_in <= {nsync + nresync, novf};   // sync errors: HLS + words dropped
    endcase
  end

endmodule
