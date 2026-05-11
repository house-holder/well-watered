.PHONY: upload uploadfs monitor run runmon

upload:
	pio run --target upload

uploadfs:
	pio run --target uploadfs

monitor:
	pio device monitor --baud 115200

size:
	pio run --target size

clean:
	pio run --target clean

compiledb:
	pio run --target compiledb

run: upload uploadfs

runmon: upload uploadfs monitor

fsmon: uploadfs monitor
