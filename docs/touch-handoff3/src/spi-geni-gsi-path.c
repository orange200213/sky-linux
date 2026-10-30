static int setup_gsi_xfer(struct spi_transfer *xfer, struct spi_geni_master *mas,
			  struct spi_device *spi_slv, struct spi_controller *spi)
{
	unsigned long flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
	struct dma_slave_config config = {};
	struct gpi_spi_config peripheral = {};
	struct dma_async_tx_descriptor *tx_desc, *rx_desc;
	int ret;

	config.peripheral_config = &peripheral;
	config.peripheral_size = sizeof(peripheral);
	peripheral.set_config = true;

	if (xfer->bits_per_word != mas->cur_bits_per_word ||
	    xfer->speed_hz != mas->cur_speed_hz) {
		mas->cur_bits_per_word = xfer->bits_per_word;
		mas->cur_speed_hz = xfer->speed_hz;
	}

	if (xfer->tx_buf && xfer->rx_buf) {
		peripheral.cmd = SPI_DUPLEX;
	} else if (xfer->tx_buf) {
		peripheral.cmd = SPI_TX;
		peripheral.rx_len = 0;
	} else if (xfer->rx_buf) {
		peripheral.cmd = SPI_RX;
		if (!(mas->cur_bits_per_word % MIN_WORD_LEN)) {
			peripheral.rx_len = ((xfer->len << 3) / mas->cur_bits_per_word);
		} else {
			int bytes_per_word = (mas->cur_bits_per_word / BITS_PER_BYTE) + 1;

			peripheral.rx_len = (xfer->len / bytes_per_word);
		}
	}

	peripheral.loopback_en = !!(spi_slv->mode & SPI_LOOP);
	peripheral.clock_pol_high = !!(spi_slv->mode & SPI_CPOL);
	peripheral.data_pol_high = !!(spi_slv->mode & SPI_CPHA);
	peripheral.cs = spi_get_chipselect(spi_slv, 0);
	peripheral.pack_en = true;
	peripheral.word_len = xfer->bits_per_word - MIN_WORD_LEN;

	ret = get_spi_clk_cfg(mas->cur_speed_hz, mas,
			      &peripheral.clk_src, &peripheral.clk_div);
	if (ret) {
		dev_err(mas->dev, "Err in get_spi_clk_cfg() :%d\n", ret);
		return ret;
	}

	/*
	 * Set fragmentation to keep CS asserted after this transfer when:
	 *  - non-last transfer with cs_change=0: keep CS asserted between chained transfers
	 *  - last transfer with cs_change=1: keep CS asserted after the message
	 *    (e.g. TPM TIS SPI uses cs_change=1 on single-transfer messages to
	 *     keep CS asserted across header, wait-state and data phases)
	 */
	peripheral.fragmentation = list_is_last(&xfer->transfer_list, &spi->cur_msg->transfers) ?
				   xfer->cs_change : !xfer->cs_change;

	if (peripheral.cmd & SPI_RX) {
		dmaengine_slave_config(mas->rx, &config);
		rx_desc = dmaengine_prep_slave_sg(mas->rx, xfer->rx_sg.sgl, xfer->rx_sg.nents,
						  DMA_DEV_TO_MEM, flags);
		if (!rx_desc) {
			dev_err(mas->dev, "Err setting up rx desc\n");
			return -EIO;
		}
	}

	/*
	 * Prepare the TX always, even for RX or tx_buf being null, we would
	 * need TX to be prepared per GSI spec
	 */
	dmaengine_slave_config(mas->tx, &config);
	tx_desc = dmaengine_prep_slave_sg(mas->tx, xfer->tx_sg.sgl, xfer->tx_sg.nents,
					  DMA_MEM_TO_DEV, flags);
	if (!tx_desc) {
		dev_err(mas->dev, "Err setting up tx desc\n");
		return -EIO;
	}

	tx_desc->callback_result = spi_gsi_callback_result;
	tx_desc->callback_param = spi;

	if (peripheral.cmd & SPI_RX)
		dmaengine_submit(rx_desc);
	dmaengine_submit(tx_desc);

	if (peripheral.cmd & SPI_RX)
		dma_async_issue_pending(mas->rx);

	dma_async_issue_pending(mas->tx);
	return 1;
}
