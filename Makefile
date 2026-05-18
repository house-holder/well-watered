.PHONY: upload uploadfs monitor run runmon size clean compiledb app firmware full

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

app: uploadfs monitor

firmware: upload monitor

full: upload uploadfs monitor
