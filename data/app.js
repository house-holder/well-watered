const zoneState = [ // countdown grace period to enable
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
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

function snapDown(mins) {
	return Math.max(15, Math.floor(mins / 15) * 15);
}

function snapUp(mins) {
	return Math.min(540, Math.ceil(mins / 15) * 15);
}

function updatePickerUI(card, zoneId) {
	const state  = zoneState[zoneId];
	const picker = card.querySelector('.duration-picker');
	const minus  = card.querySelector('.dur-btn.minus');
	const plus   = card.querySelector('.dur-btn.plus');
	const disp   = card.querySelector('.duration-display');

	if (state.mode === 'idle') {
		picker.style.display = 'flex';
		minus.classList.remove('hidden');
		plus.classList.remove('hidden');
		picker.classList.remove('countdown-active');
		disp.textContent = fmtDuration(state.duration);
	} else if (state.mode === 'override') {
		minus.classList.add('hidden');
		plus.classList.add('hidden');
		picker.classList.add('countdown-active');
	} else if (state.mode === 'grace') {
		minus.classList.add('hidden');
		plus.classList.add('hidden');
	} else if (state.mode === 'scheduled' || state.mode === 'paused') {
		picker.style.display = 'none';
	}
}

function fmtTime(timeStr) {
	const parts = timeStr.split(':');
	const raw = parseInt(parts[0]);
	const minutes = parts[1];
	const suffix = raw >= 12 ? 'PM' : 'AM';
	const hours = raw % 12 || 12;
	return `${hours}:${minutes} ${suffix}`
}

function fmtDuration(mins) {
	const h = Math.floor(mins / 60);
	const m = mins % 60;
	return `${h}:${String(m).padStart(2, '0')}`;
}

function fmtCountdown(seconds) {
	const h = Math.floor(seconds / 3600);
	const m = Math.floor((seconds % 3600) / 60);
	const s = seconds % 60;
	return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
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
			btn.classList.add('active');
			btn.textContent = zone.mode === 'scheduled' ? 'Pause' : 'Disable';
			zoneState[zone.id].mode = zone.mode;

			if (zone.mode === 'override' && zone.stopTime > 0) {
				if (zoneState[zone.id].countdown) {
					clearInterval(zoneState[zone.id].countdown);
					zoneState[zone.id].countdown = null;
				}
				const remaining = Math.max(
					0, zone.stopTime - Math.floor(Date.now() / 1000)
				);
				startCountdown(card, zone.id, remaining);
			}

		} else if (zone.mode === 'paused') {
			badge.textContent = 'OFF';
			badge.classList.remove('on');
			since.textContent = '';
			btn.textContent = 'Resume';
			btn.classList.remove('active');
			zoneState[zone.id].mode = 'paused';

		} else {
			badge.textContent = 'OFF';
			badge.classList.remove('on');
			since.textContent = '';
			btn.textContent = 'Enable';
			btn.classList.remove('active');
			zoneState[zone.id].mode = 'idle';
		}

		updatePickerUI(card, zone.id);
	});
}

function startCountdown(card, zoneId, initialSeconds) {
	const state = zoneState[zoneId];
	if (state.countdown) {
		clearInterval(state.countdown);
		state.countdown = null;
	}

	let remaining = initialSeconds;
	const disp  = card.querySelector('.duration-display');
	disp.textContent = fmtCountdown(remaining);

	updatePickerUI(card, zoneId);

	state.countdown = setInterval(async () => {
		remaining--;
		disp.textContent = fmtCountdown(remaining);

		if (remaining <= 0) {
			clearInterval(state.countdown);
			state.countdown = null;
			await fetch(`/api/zones/${zoneId}/disable`, { method: 'POST' });
			const data = await fetchState();
			applyState(data);
			state.mode = 'idle';
			updatePickerUI(card, zoneId);
		}
	}, 1000);
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

		const startPicker = card.querySelector('.time-picker[data-type="start"]');
		const startHour12 = parseInt(startPicker.querySelector('.t-hour').value);
		const startMin  = parseInt(startPicker.querySelector('.t-minute').value);
		const startAmpm = startPicker.querySelector('.t-ampm').value;
		const startHour = startAmpm === 'PM'
			? (startHour12 % 12) + 12
			: startHour12 % 12;

		const stopPicker = card.querySelector('.time-picker[data-type="stop"]');
		const stopHour12 = parseInt(stopPicker.querySelector('.t-hour').value);
		const stopMin  = parseInt(stopPicker.querySelector('.t-minute').value);
		const stopAmpm = stopPicker.querySelector('.t-ampm').value;
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

		const startPicker = card.querySelector('.time-picker[data-type="start"]');
		const stopPicker = card.querySelector('.time-picker[data-type="stop"]');

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
			const card = document.querySelector(`.zone-card[data-zone="${zoneId}"]`);
		
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
							await fetch(`/api/zones/${zoneId}/enable`, {
								method: 'POST',
								headers: { 'Content-Type': 'application/json' },
								body: JSON.stringify({ duration: state.duration })
							});
							const data = await fetchState();
							applyState(data);
							btn.textContent = 'Disable';
							startCountdown(card, zoneId, state.duration * 60);
						}
					}, 1000);

				} else {
					clearInterval(state.timer);
					await fetch(`/api/zones/${zoneId}/enable`, {
						method: 'POST',
						headers: { 'Content-Type': 'application/json' },
						body: JSON.stringify({ duration: state.duration })
					});
					const data = await fetchState();
					applyState(data);
					btn.textContent = 'Disable';
					startCountdown(card, zoneId, state.duration * 60);
				}

			} else if (state.mode === 'override') {
				if (state.countdown) {
					console.log('countdown ref: ', state.countdown);
					clearInterval(state.countdown);
					state.countdown = null;
					console.log('cleared');
				} else {
					console.log('countdown was null, nothing to clear');
				}
				await fetch(`/api/zones/${zoneId}/disable`, { method: 'POST' });
				const data = await fetchState();
				applyState(data);

			} else if (state.mode === 'paused') {
				await fetch(`/api/zones/${zoneId}/resume`, { method: 'POST' });
				let attempts = 0;
				const poll = setInterval(async () => {
					attempts++;
					const data = await fetchState();
					applyState(data);
					if (zoneState[zoneId].mode === 'scheduled' || attempts > 10) {
						clearInterval(poll);
					}
				}, 100);

			} else if (state.mode === 'scheduled') {
				await fetch(`/api/zones/${zoneId}/pause`, { method: 'POST' });
				const data = await fetchState();
				applyState(data);

			} else if (state.mode === 'grace') {
				clearInterval(state.timer);
				btn.textContent = 'Enable';
				state.mode = 'idle';
				updatePickerUI(card, zoneId);
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

		card.querySelector('.minus').addEventListener('click', () => {
			const state = zoneState[zoneId];
			if (state.mode !== 'idle') return;
			const snapped = snapDown(state.duration);
			state.duration = snapped < state.duration ?
				snapped : Math.max(15, snapped - 15);
			card.querySelector('.duration-display').textContent =
				fmtDuration(state.duration);
		});

		card.querySelector('.plus').addEventListener('click', () => {
			const state = zoneState[zoneId];
			if (state.mode !== 'idle') return;
			const snapped = snapUp(state.duration);
			state.duration = snapped > state.duration ?
				snapped : Math.min(540, snapped + 15);
			card.querySelector('.duration-display').textContent =
				fmtDuration(state.duration);
		});
	});
}

document.addEventListener('visibilitychange', () => {
	if (document.visibilityState === 'visible') {
		fetchState().then(applyState);
	}
});

init();
updateClock();
setInterval(updateClock, 1000);

