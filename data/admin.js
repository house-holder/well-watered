'use strict';

function fmt(n) { return Number(n).toFixed(1); }

// Resources ------------------------------------------------------------------
let resInterval = null;

async function loadResources() {
	try {
		const d = await fetch('/api/resources').then(r => r.json());
		const h = d.heap, u = d.uptime, f = d.fs;
		const heapPct = (h.used / h.total * 100).toFixed(0);
		const fsPct = (f.used / f.total * 100).toFixed(0);

		document.getElementById('res-heap-val').textContent =
			`${fmt(h.used)} / ${fmt(h.total)} KiB (${heapPct}%)`;
		document.getElementById('res-heap-bar').style.width =
			`${heapPct}%`;
		document.getElementById('res-peak-val').textContent =
			`${fmt(h.peak)} KiB`;
		document.getElementById('res-block-val').textContent =
			`${fmt(h.maxBlock)} KiB`;
		document.getElementById('res-fs-val').textContent =
			`${fmt(f.used)} / ${fmt(f.total)} KiB (${fsPct}%)`;
		document.getElementById('res-fs-bar').style.width = `${fsPct}%`;

		const mm = String(u.minutes).padStart(2, '0');
		const ss = String(u.seconds).padStart(2, '0');
		let uptimeString = "";
		if (u.days !== 0) {
			uptimeString = `${u.days}d ${u.hours}h ${mm}m`;
		} else {
			uptimeString = `${u.hours}h ${mm}m ${ss}s`;
		}
		document.getElementById('res-uptime-val').textContent = uptimeString;
	} catch (e) {
		document.getElementById('res-uptime-val').textContent =
			'Error loading resources.';
	}
}

document.getElementById('res-refresh').addEventListener('click', loadResources);
document.getElementById('res-auto').addEventListener('change', function() {
	clearInterval(resInterval);
	resInterval = this.checked ? setInterval(loadResources, 5000) : null;
});

// Log Viewer -----------------------------------------------------------------
let logInterval = null;

async function loadLogs() {
	const count = document.getElementById('log-count').value;
	const pre = document.getElementById('log-output');
	try {
		const text = await fetch(`/api/logs/last/${count}`).then(r => r.text());
		pre.textContent = text.trim() || '(empty)';
		pre.scrollTop = pre.scrollHeight;
	} catch (e) {
		pre.textContent = 'Error loading logs.';
	}
}

document.getElementById('log-refresh').addEventListener('click', loadLogs);
document.getElementById('log-count').addEventListener('change', loadLogs);
document.getElementById('log-auto').addEventListener('change', function() {
	clearInterval(logInterval);
	logInterval = this.checked ? setInterval(loadLogs, 10000) : null;
});

// Filesystem (includes log files) --------------------------------------------
const LOG_FILE_NAMES = new Set(['/log0.txt', '/log1.txt']);

async function loadFS() {
	const list = document.getElementById('fs-list');
	try {
		const [fsData, logData] = await Promise.all([
			fetch('/api/fs').then(r => r.json()),
			fetch('/api/logs/state').then(r => r.json()),
		]);

		list.innerHTML = '';
		document.getElementById('fs-file-count').textContent =
			`${fsData.files.length} files`;

		const activeLogName = `/log${logData.active}.txt`;
		const inactiveIdx = logData.inactive;

		for (const f of fsData.files) {
			const name = f.name.startsWith('/') ? f.name : '/' + f.name;
			const isLog = LOG_FILE_NAMES.has(name);
			const isActive = name === activeLogName;

			const row = document.createElement('div');
			row.className = 'file-row';
			const sizeStr = `${fmt(f.size / 1024)} KiB`;

			if (isLog) {
				if (isActive) {
					row.innerHTML = `
						<span class="file-name">${name}</span>
						<span class="file-size">${sizeStr}</span>
						<span class="badge-active">active</span>`;
				} else {
					row.innerHTML = `
						<span class="file-name">${name}</span>
						<span class="file-size">${sizeStr}</span>
						<button class="btn-danger" data-idx="${inactiveIdx}">
							Clear
						</button>`;

					row.querySelector('.btn-danger').addEventListener('click',
						async function() {
							const idx = this.dataset.idx;
							if (!confirm(`Clear log${idx}.txt?`)) return;
							const res = await fetch(`/api/logs/delete/${idx}`, {
								method: 'POST'
							});
							if (!res.ok) {
								const err = await res.json().catch(() => ({}));
								alert(`Error: ${err.error || res.status}`);
							}
							loadFS();
						});
				}
			} else {
				row.innerHTML = `
					<span class="file-name">${name}</span>
					<span class="file-size">${sizeStr}</span>`;
			}

			list.appendChild(row);
		}
	} catch (e) {
		list.textContent = 'Error loading filesystem.';
	}
}

document.getElementById('fs-refresh').addEventListener('click', loadFS);

loadResources();
loadLogs();
loadFS();
