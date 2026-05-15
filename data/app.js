const zoneState = [ // countdown grace period to enable
	{ mode: 'idle', timer: null, remaining: 0 },
	{ mode: 'idle', timer: null, remaining: 0 },
	{ mode: 'idle', timer: null, remaining: 0 },
];

const graceCountdown = 10;

// setup functions -----------------------------------------------------------------------
function updateClock() {
    const now = new Date();
    const raw = now.getHours();
    const suffix = raw >= 12 ? 'PM' : 'AM';
    const h = raw % 12 || 12;
    const m = String(now.getMinutes()).padStart(2, '0');
    const s = String(now.getSeconds()).padStart(2, '0');
    document.getElementById('clock').textContent = `${h}:${m}:${s} ${suffix}`;
}

function populateTimePickers() {
	document.querySelectorAll('.t-hour').forEach(select => {
		for (let i = 1; i <= 12; i++) {
			const opt = document.createElement('option');
			opt.value = i;
			opt.textContent = i;
			select.appendChild(opt);
		}
	});

	document.querySelectorAll('.t-minute').forEach(select => {
		for (let i = 0; i <= 59; i++) {
			const opt = document.createElement('option');
			opt.value = i;
			opt.textContent = String(i).padStart(2, '0');
			select.appendChild(opt);
		}
	});	
}

function formatTime(timeStr) {
	const parts = timeStr.split(':');
	const raw = parseInt(parts[0]);
	const minutes = parts[1];
	const suffix = raw >= 12 ? 'PM' : 'AM';
	const hours = raw % 12 || 12;
	return `${hours}:${minutes} ${suffix}`
}

// active functions ----------------------------------------------------------------------
async function fetchState() {
	const resp = await fetch('/api/state');
	const data = await resp.json();
	return data;
}

function applyState(data) {
	data.zones.forEach(zone => {
		const card = document.querySelector(`.zone-card[data-zone="${zone.id}"]`);
		if (!card) return;

		const badge = card.querySelector('.status-badge');
		const since = card.querySelector('.status-since');
		const btn = card.querySelector('.override-btn');
		
		if (zone.running) {
			badge.textContent = 'ON';
			badge.classList.add('on');
			since.textContent = zone.since ? `since ${formatTime(zone.since)}` : '';
			btn.textContent = 'Disable';
			btn.classList.add('active');
		} else {
			badge.textContent = 'OFF';
			badge.classList.remove('on');
			since.textContent = '';
			btn.textContent = 'Enable';
			btn.classList.remove('active');
		}
	});
}

async function init() {
	populateTimePickers();
	const data = await fetchState();
	applyState(data);

    document.querySelectorAll('.override-btn').forEach(btn => {
        btn.addEventListener('click', async () => {
            const zoneId = parseInt(btn.dataset.zone);
			const state = zoneState[zoneId];
			const testingEnabled = false;
			
			if (state.mode === 'idle') {
				if (!testingEnabled) {
					state.mode = 'grace';
					state.remaining = graceCountdown;
					btn.textContent = `${state.remaining}s... Cancel`;

					state.timer = setInterval(async () => {
						state.remaining--;
						if (state.remaining > 0) {
							btn.textContent = `${state.remaining}s... Cancel`;
						} else {
							clearInterval(state.timer);
							await fetch(`/api/zones/${zoneId}/enable`, { method: 'POST' });
							const data = await fetchState();
							applyState(data);
							state.mode = 'active';
							btn.textContent = 'Disable';
						}
					}, 1000);
				} else {
					clearInterval(state.timer);
					await fetch(`/api/zones/${zoneId}/enable`, { method: 'POST' });
					const data = await fetchState();
					applyState(data);
					state.mode = 'active';
					btn.textContent = 'Disable';
				}
			} else if (state.mode === 'grace') {
				clearInterval(state.timer);
				btn.textContent = 'Enable';
				state.mode = 'idle';
			} else if (state.mode === 'active') {
				await fetch(`/api/zones/${zoneId}/disable`, { method: 'POST' });
				const data = await fetchState();
				applyState(data);
				state.mode = 'idle';
			}
        });
    });
}

async function fetchSchedule() {
	const resp = await fetch('/api/schedule');
	const data = await resp.json();
	return data;
}

function applySchedule(data) {
	// find card by 'data-zone'
	// for (let i = 0; i < 3; i++) {
	//
	// }
	// set correct day buttons to 'active' class
	// set start/stop time pickers to stored values
}

init();

updateClock();
setInterval(updateClock, 1000);
