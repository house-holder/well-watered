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

updateClock();
populateTimePickers();
setInterval(updateClock, 1000);
