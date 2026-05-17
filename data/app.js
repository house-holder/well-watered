const zoneState = [ // countdown grace period to enable
	{ mode: 'idle', timer: null, remaining: 0 },
	{ mode: 'idle', timer: null, remaining: 0 },
	{ mode: 'idle', timer: null, remaining: 0 },
];

const graceEnabled = false;
const graceCountdown = 5;

// setup functions ------------------------------------------------------------
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

function fmtTime(timeStr) {
	const parts = timeStr.split(':');
	const raw = parseInt(parts[0]);
	const minutes = parts[1];
	const suffix = raw >= 12 ? 'PM' : 'AM';
	const hours = raw % 12 || 12;
	return `${hours}:${minutes} ${suffix}`
}

// active functions -----------------------------------------------------------
async function fetchState() {
	const resp = await fetch('/api/state');
	const data = await resp.json();
	return data;
}

function applyState(data) {
	data.zones.forEach(zone => {
		const card = document.querySelector(
			`.zone-card[data-zone="${zone.id}"]`
		);
		if (!card) return;

		const badge = card.querySelector('.status-badge');
		const since = card.querySelector('.status-since');
		const btn = card.querySelector('.override-btn');
		
		if (zone.running) {
			badge.textContent = 'ON';
			badge.classList.add('on');
			since.textContent = zone.since ? `${fmtTime(zone.since)}` : '';
			btn.textContent = 'Disable';
			btn.classList.add('active');
			zoneState[zone.id].mode = 'active';
		} else {
			badge.textContent = 'OFF';
			badge.classList.remove('on');
			since.textContent = '';
			btn.textContent = 'Enable';
			btn.classList.remove('active');
			zoneState[zone.id].mode = 'idle';
		}
	});
}

async function fetchSchedule() {
	const resp = await fetch('/api/schedule');
	const data = await resp.json();
	return data;
}

async function saveSchedule() {
    const payload = { zones: [] };

    document.querySelectorAll('.zone-card').forEach(card => {
        const id = parseInt(card.dataset.zone);
	
        const days = [...card.querySelectorAll('.day')]
            .map(btn => btn.classList.contains('active'));

        const pickerA = card.querySelector('.time-picker[data-type="start"]');
        const startHour12 = parseInt(pickerA.querySelector('.t-hour').value);
        const startMin  = parseInt(pickerA.querySelector('.t-minute').value);
        const startAmpm = pickerA.querySelector('.t-ampm').value;
        const startHour = startAmpm === 'PM'
			? (startHour12 % 12) + 12
			: startHour12 % 12;

        const pickerB = card.querySelector('.time-picker[data-type="stop"]');
        const stopHour12 = parseInt(pickerB.querySelector('.t-hour').value);
        const stopMin  = parseInt(pickerB.querySelector('.t-minute').value);
        const stopAmpm = pickerB.querySelector('.t-ampm').value;
		const stopHour = stopAmpm === 'PM'
			? (stopHour12 % 12) + 12
			: stopHour12 % 12;

        payload.zones.push({id, days, startHour, startMin, stopHour, stopMin});
    });

    await fetch('/api/schedule/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    });
}

function setTimePicker(picker, hour24, minute) {
	const hour12 = hour24 % 12 || 12;
	const ampm = hour24 >= 12 ? 'PM' : 'AM';
	picker.querySelector('.t-ampm').value = ampm;
	picker.querySelector('.t-hour').value = hour12;
	picker.querySelector('.t-minute').value = minute;
}

function applySchedule(data) {
	data.zones.forEach(zone => {
		const s0 = `.zone-card[data-zone="${zone.id}"]`;
		const card = document.querySelector(s0);
		if (!card) return;

		const dayBtns = card.querySelectorAll('.day');
		zone.days.forEach((active, index) => {
			dayBtns[index].classList.toggle('active', active);
		});

		const s1 = '.time-picker[data-type="start"]';
		const startPicker = card.querySelector(s1);

		const s2 = '.time-picker[data-type="stop"]';
		const stopPicker = card.querySelector(s2);

		setTimePicker(startPicker, zone.startHour, zone.startMin);
		setTimePicker(stopPicker, zone.stopHour, zone.stopMin);
	});
}

async function init() {
	populateTimePickers();

	const data = await fetchState();
	applyState(data);

	const schedule = await fetchSchedule();
	applySchedule(schedule);

    document.querySelectorAll('.override-btn').forEach(btn => {
        btn.addEventListener('click', async () => {
            const zoneId = parseInt(btn.dataset.zone);
			const state = zoneState[zoneId];
			
			if (state.mode === 'idle') {
				if (graceEnabled) {
					state.mode = 'grace';
					state.remaining = graceCountdown;
					btn.textContent = `${state.remaining}s... Cancel`;

					state.timer = setInterval(async () => {
						state.remaining--;
						if (state.remaining > 0) {
							btn.textContent = `${state.remaining}s... Cancel`;
						} else {
							clearInterval(state.timer);
							await fetch(
								`/api/zones/${zoneId}/enable`,
								{ method: 'POST' }
							);
							const data = await fetchState();
							applyState(data);
							state.mode = 'active';
							btn.textContent = 'Disable';
						}
					}, 1000);
				} else {
					clearInterval(state.timer);
					await fetch(
						`/api/zones/${zoneId}/enable`,
						{ method: 'POST' }
					);
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
				await fetch(
					`/api/zones/${zoneId}/disable`,
					{ method: 'POST' }
				);
				const data = await fetchState();
				applyState(data);
				state.mode = 'idle';
			}
        });
    });

	document.querySelectorAll('.zone-card').forEach(card => {
		const zoneId = parseInt(card.dataset.zone);

		card.querySelectorAll('.day').forEach(btn => {
			btn.addEventListener('click', () => {
				btn.classList.toggle('active');
				saveSchedule();
			});
		});
		card.querySelectorAll('.t-hour, .t-minute, .t-ampm').forEach(select => {
			select.addEventListener('change', () => saveSchedule());
		});
	});
}

init();

updateClock();
setInterval(updateClock, 1000);
