const zoneState = [ // countdown grace period to enable
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
	{ mode: 'idle', timer: null, remaining: 0, duration: 120, countdown: null },
];

const graceEnabled = false;
const graceCountdown = 5;
let saveTimer = null;

function debounceScheduleSave() {
	clearTimeout(saveTimer);
	saveTimer = setTimeout(() => {
		saveTimer = null;
		saveSchedule();
	}, 8000);
}

function flushScheduleSave() {
	if (saveTimer === null) return;   // nothing pending
	clearTimeout(saveTimer);
	saveTimer = null;
	saveSchedule();
}

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

function snapDown(mins) {
	return Math.max(15, Math.floor(mins / 15) * 15);
}

function snapUp(mins) {
	return Math.min(540, Math.ceil(mins / 15) * 15);
}

function updatePickerUI(card, zoneId) {
	const state = zoneState[zoneId];
	const picker = card.querySelector('.duration-picker');
	const minus = card.querySelector('.dur-btn.minus');
	const plus = card.querySelector('.dur-btn.plus');
	const disp = card.querySelector('.duration-display');

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
	const disp = card.querySelector('.duration-display');
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

		const startVal = card.querySelector(
			'.time-input[data-type="start"]').value;
		const [startHour, startMin] = startVal.split(':').map(Number);

		const stopVal = card.querySelector(
			'.time-input[data-type="stop"]').value;
		const [stopHour, stopMin] = stopVal.split(':').map(Number);

		if (!startVal || !stopVal) return;
		payload.zones.push({
			id, days, startHour, startMin, stopHour, stopMin
		});
	});

	await fetch('/api/schedule/save', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify(payload)
	});
}

function setTimePicker(input, hour24, minute) {
	const h = String(hour24).padStart(2, '0');
	const m = String(minute).padStart(2, '0');
	input.value = `${h}:${m}`;
}

function applySchedule(data) {
	data.zones.forEach(zone => {
		const card = document.querySelector(
			`.zone-card[data-zone="${zone.id}"]`
		);
		if (!card) return;

		const dayBtns = card.querySelectorAll('.day');
		zone.days.forEach((active, index) => {
			dayBtns[index].classList.toggle('active', active);
		});

		const startInput = card.querySelector(
			'.time-input[data-type="start"]'
		);
		const stopInput = card.querySelector(
			'.time-input[data-type="stop"]'
		);
		setTimePicker(startInput, zone.startHour, zone.startMin);
		setTimePicker(stopInput, zone.stopHour, zone.stopMin);
	});
}

async function init() {
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
					clearInterval(state.countdown);
					state.countdown = null;
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
				debounceScheduleSave();
			});
		});

		card.querySelectorAll('.time-input').forEach(input => {
			input.addEventListener('change', () => debounceScheduleSave());
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
	} else {
		flushScheduleSave();
	}
});

window.addEventListener('pagehide', flushScheduleSave)

const clockMenu = document.getElementById('clock-menu');
document.getElementById('clock-wrap').addEventListener('click', (e) => {
	e.stopPropagation();
	clockMenu.hidden = !clockMenu.hidden;
});
document.addEventListener('click', () => { clockMenu.hidden = true; });

init();
updateClock();
setInterval(updateClock, 1000);
