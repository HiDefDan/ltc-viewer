const BASE_DISPLAY_WIDTH = 1920;
const BASE_DISPLAY_HEIGHT = 1080;
const BASE_TIME_STRING_HEIGHT = 256;

class ConfigManager {
    constructor() {
        this.config = {};
        this.tzdata = {};
        this.init();
    }

    getGlyphScale(width, height) {
        const scaleX = width / BASE_DISPLAY_WIDTH;
        const scaleY = height / BASE_DISPLAY_HEIGHT;
        return Math.min(scaleX, scaleY);
    }

    getScaledStringHeight(width, height) {
        const scale = this.getGlyphScale(width, height);
        return Math.max(1, Math.round(BASE_TIME_STRING_HEIGHT * scale));
    }

    getMaxYOffsetForResolution(width, height) {
        const stringHeight = this.getScaledStringHeight(width, height);
        return Math.max(0, (height - stringHeight) / 2);
    }

    clampTimecodeYOffset(value, width, height) {
        if (Number.isNaN(value)) return 0;
        const maxOffset = this.getMaxYOffsetForResolution(width, height);
        return Math.min(maxOffset, Math.max(-maxOffset, value));
    }

    getMaxXOffsetForResolution(width, height) {
        return Math.max(0, width / 2);
    }

    clampTimecodeXOffset(value, width, height) {
        if (Number.isNaN(value)) return 0;
        const maxOffset = this.getMaxXOffsetForResolution(width, height);
        return Math.min(maxOffset, Math.max(-maxOffset, value));
    }

    parseResolutionValue(value) {
        const match = /^([0-9]+)x([0-9]+)$/.exec(value || '');
        if (!match) {
            return { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT };
        }
        return { width: parseInt(match[1]), height: parseInt(match[2]) };
    }

    updateTimecodeYBounds(width, height) {
        const yInput = document.getElementById('timecodeYOffset');
        const maxOffset = this.getMaxYOffsetForResolution(width, height);
        yInput.max = String(Math.floor(maxOffset));
        yInput.min = String(-Math.floor(maxOffset));
        yInput.value = this.clampTimecodeYOffset(parseInt(yInput.value), width, height);
    }

    updateTimecodeXBounds(width, height) {
        const xInput = document.getElementById('timecodeXOffset');
        const maxOffset = this.getMaxXOffsetForResolution(width, height);
        xInput.max = String(Math.floor(maxOffset));
        xInput.min = String(-Math.floor(maxOffset));
        xInput.value = this.clampTimecodeXOffset(parseInt(xInput.value), width, height);
    }

    async init() {
        this.setupEventListeners();
        await this.loadTimezoneData();
        this.populateRegions();
        await this.loadConfig();
        await this.loadNtpServers();
        await this.loadDisplayModes();
        this.startNtpStatusRefresh();
    }

    setupEventListeners() {
        const form = document.getElementById('configForm');
        form.addEventListener('submit', (e) => this.handleSubmit(e));

        document.getElementById('resetBtn').addEventListener('click', () => this.resetToDefaults());

        // Region selector
        document.getElementById('region').addEventListener('change', (e) => {
            this.updateTimezoneSelect(e.target.value);
        });

        document.getElementById('displayResolution').addEventListener('change', (e) => {
            const selected = this.parseResolutionValue(e.target.value);
            this.populateRefreshRateSelect(selected.width, selected.height);
            this.updateTimecodeYBounds(selected.width, selected.height);
            this.updateTimecodeXBounds(selected.width, selected.height);
        });

        // Color sliders
        ['colorR', 'colorG', 'colorB'].forEach(id => {
            document.getElementById(id).addEventListener('input', (e) => {
                document.getElementById(id + 'Value').value = e.target.value;
                this.updateColorPreview('colorPreview');
            });
        });

        // Add custom NTP server
        document.getElementById('addCustomServerBtn').addEventListener('click', () => {
            this.addCustomNtpServer();
        });

        // Allow Enter key in custom server input
        document.getElementById('ntpCustomServer').addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                this.addCustomNtpServer();
            }
        });

    }

    async loadTimezoneData() {
        try {
            const response = await fetch('/api/tzdata');
            this.tzdata = await response.json();
        } catch (e) {
            console.error('Failed to load timezone data:', e);
            this.showStatus('Failed to load timezone data', 'error');
        }
    }

    async loadConfig() {
        try {
            const response = await fetch('/api/config');
            this.config = await response.json();
            this.populateForm();
        } catch (e) {
            console.error('Failed to load config:', e);
            this.showStatus('Failed to load configuration', 'error');
        }
    }

    async loadNtpServers() {
        try {
            const response = await fetch('/api/ntp-servers');
            const data = await response.json();
            this.ntpServers = data;
            this.populateNtpServerSelect();
        } catch (e) {
            console.error('Failed to load NTP servers:', e);
        }
    }

    async loadDisplayModes() {
        try {
            const response = await fetch('/api/display-modes');
            const data = await response.json();
            if (data.modes && data.modes.length > 0) {
                this.displayModes = data.modes;
                this.preferredMode = (typeof data.preferred_index === 'number' && data.modes[data.preferred_index])
                    ? data.modes[data.preferred_index]
                    : null;
                this.populateResolutionSelect();
            } else {
                console.error('No display modes returned from API');
            }
        } catch (e) {
            console.error('Failed to load display modes:', e);
            this.displayModes = [
                { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT, refresh: 50.00, preferred: false },
                { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT, refresh: 59.94, preferred: false },
                { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT, refresh: 60.00, preferred: false }
            ];
            this.preferredMode = null;
            this.populateResolutionSelect();
        }
    }

    populateResolutionSelect() {
        const resolutionSelect = document.getElementById('displayResolution');
        resolutionSelect.innerHTML = '';

        const resolutions = [];
        this.displayModes.forEach(mode => {
            const key = `${mode.width}x${mode.height}`;
            if (!resolutions.find(r => r.key === key)) {
                resolutions.push({
                    key,
                    width: mode.width,
                    height: mode.height,
                    preferred: !!mode.preferred
                });
            } else if (mode.preferred) {
                const existing = resolutions.find(r => r.key === key);
                existing.preferred = true;
            }
        });

        resolutions.sort((a, b) => (b.width * b.height) - (a.width * a.height));

        resolutions.forEach(resolution => {
            const option = document.createElement('option');
            option.value = resolution.key;
            option.textContent = `${resolution.width}x${resolution.height}${resolution.preferred ? ' (Preferred)' : ''}`;
            resolutionSelect.appendChild(option);
        });

        let targetResolution = `${this.config.display_width || BASE_DISPLAY_WIDTH}x${this.config.display_height || BASE_DISPLAY_HEIGHT}`;
        if (!resolutions.find(r => r.key === targetResolution)) {
            if (this.preferredMode) {
                targetResolution = `${this.preferredMode.width}x${this.preferredMode.height}`;
            } else if (resolutions.length > 0) {
                targetResolution = resolutions[0].key;
            }
        }

        resolutionSelect.value = targetResolution;
        const selected = this.parseResolutionValue(targetResolution);
        this.populateRefreshRateSelect(selected.width, selected.height);
        this.updateTimecodeYBounds(selected.width, selected.height);
    }

    populateRefreshRateSelect(width, height) {
        const select = document.getElementById('refreshHz');
        select.innerHTML = '';

        const matchingModes = this.displayModes
            .filter(mode => mode.width === width && mode.height === height)
            .sort((a, b) => b.refresh - a.refresh);

        matchingModes.forEach(mode => {
            const option = document.createElement('option');
            option.value = Number(mode.refresh).toFixed(2);
            
            if (Math.abs(mode.refresh - 59.94) < 0.01) {
                option.textContent = `${Number(mode.refresh).toFixed(2)} Hz (NTSC)`;
            } else if (Math.abs(mode.refresh - 50.0) < 0.01) {
                option.textContent = `${Number(mode.refresh).toFixed(2)} Hz (PAL)`;
            } else {
                option.textContent = `${Number(mode.refresh).toFixed(2)} Hz`;
            }

            if (mode.preferred) {
                option.textContent += ' (Preferred)';
            }
            
            select.appendChild(option);
        });

        if (matchingModes.length === 0) {
            const fallback = document.createElement('option');
            fallback.value = '50.00';
            fallback.textContent = '50.00 Hz (Fallback)';
            select.appendChild(fallback);
            select.value = '50.00';
            return;
        }

        const configHz = parseFloat(this.config.refresh_hz || 50.0);
        let best = matchingModes[0];
        let bestDiff = Math.abs(best.refresh - configHz);
        for (const mode of matchingModes) {
            const diff = Math.abs(mode.refresh - configHz);
            if (diff < bestDiff) {
                best = mode;
                bestDiff = diff;
            }
        }

        select.value = Number(best.refresh).toFixed(2);
    }

    populateNtpServerSelect() {
        if (!this.ntpServers || !this.ntpServers.servers) return;
        
        const select = document.getElementById('ntpServer');
        select.innerHTML = '';

        // Add built-in servers
        const builtins = this.ntpServers.servers.filter(s => s.type === 'builtin');
        if (builtins.length > 0) {
            const builtinGroup = document.createElement('optgroup');
            builtinGroup.label = 'Built-in Stratum 0 Servers';
            builtins.forEach(server => {
                const option = document.createElement('option');
                option.value = server.name;
                option.textContent = server.name;
                builtinGroup.appendChild(option);
            });
            select.appendChild(builtinGroup);
        }

        // Add custom servers
        const customs = this.ntpServers.servers.filter(s => s.type === 'custom');
        if (customs.length > 0) {
            const customGroup = document.createElement('optgroup');
            customGroup.label = 'Custom Servers';
            customs.forEach(server => {
                const option = document.createElement('option');
                option.value = server.name;
                option.textContent = server.name;
                customGroup.appendChild(option);
            });
            select.appendChild(customGroup);
        }

        // Set current selection
        if (this.ntpServers.current) {
            select.value = this.ntpServers.current;
        }

        // Populate custom servers list
        this.populateCustomServersList(customs);
    }

    populateCustomServersList(customServers) {
        const listEl = document.getElementById('customServersList');
        listEl.innerHTML = '';

        if (!customServers || customServers.length === 0) {
            listEl.innerHTML = '<p style="color: #888; font-size: 12px; margin: 0;">No custom servers yet</p>';
            return;
        }

        customServers.forEach(server => {
            const item = document.createElement('div');
            item.className = 'custom-server-item';
            item.innerHTML = `
                <div>
                    <span class="server-name">${server.name}</span>
                </div>
                <button type="button" class="delete-btn" data-server="${server.name}">Delete</button>
            `;
            
            item.querySelector('.delete-btn').addEventListener('click', () => {
                this.deleteCustomNtpServer(server.name);
            });
            
            listEl.appendChild(item);
        });
    }

    addCustomNtpServer() {
        const input = document.getElementById('ntpCustomServer');
        const server = input.value.trim();
        
        if (!server) {
            this.showStatusWithAutoHide('Please enter a server name or IP address', 'error');
            return;
        }

        // Validate: basic check for valid hostname or IP
        if (!/^[\w.-]+$/.test(server)) {
            this.showStatusWithAutoHide('Invalid server name or IP format', 'error');
            return;
        }

        // Add to custom servers list in config
        let customServers = [];
        try {
            customServers = JSON.parse(this.config.custom_ntp_servers || '[]');
        } catch (e) {
            customServers = [];
        }

        if (customServers.includes(server)) {
            this.showStatusWithAutoHide('Server already exists in custom list', 'error');
            return;
        }

        customServers.push(server);
        this.config.custom_ntp_servers = JSON.stringify(customServers);
        
        this.showStatus('Adding custom server...', 'loading');
        input.value = '';
        
        // Save to backend FIRST, then reload dropdown after successful save
        this.saveConfig()
            .then(() => {
                // Now fetch the updated list from API after save is complete
                return this.loadNtpServers();
            })
            .then(() => {
                this.showStatusWithAutoHide('Custom server added!', 'success', 2000);
            })
            .catch(err => {
                console.error('Failed to add custom server:', err);
                this.showStatusWithAutoHide('Failed to add custom server', 'error');
                // Rollback local change since save failed
                customServers = customServers.filter(s => s !== server);
                this.config.custom_ntp_servers = JSON.stringify(customServers);
            });
    }

    deleteCustomNtpServer(serverName) {
        if (!confirm(`Delete custom server "${serverName}"?`)) {
            return;
        }

        // Remove from custom servers list
        let customServers = [];
        try {
            customServers = JSON.parse(this.config.custom_ntp_servers || '[]');
        } catch (e) {
            customServers = [];
        }

        customServers = customServers.filter(s => s !== serverName);
        this.config.custom_ntp_servers = JSON.stringify(customServers);

        // If the deleted server was selected, pick the first built-in
        if (this.config.ntp_server === serverName) {
            this.config.ntp_server = 'pool.ntp.org';
        }

        this.showStatus('Deleting server...', 'loading');

        // Save to backend FIRST, then reload dropdown after successful save
        this.saveConfig()
            .then(() => {
                // Now fetch the updated list from API after save is complete
                return this.loadNtpServers();
            })
            .then(() => {
                this.showStatusWithAutoHide('Custom server deleted', 'success', 2000);
            })
            .catch(err => {
                console.error('Failed to delete custom server:', err);
                this.showStatusWithAutoHide('Failed to delete server', 'error');
            });
    }

    async saveConfig() {
        const selectedResolution = this.parseResolutionValue(document.getElementById('displayResolution').value);
        const config = {
            timezone: document.getElementById('timezone').value,
            ntp_server: document.getElementById('ntpServer').value,
            display_width: selectedResolution.width,
            display_height: selectedResolution.height,
            refresh_hz: parseFloat(document.getElementById('refreshHz').value),
            timecode_x_offset: this.clampTimecodeXOffset(parseInt(document.getElementById('timecodeXOffset').value), selectedResolution.width, selectedResolution.height),
            timecode_y_offset: this.clampTimecodeYOffset(parseInt(document.getElementById('timecodeYOffset').value), selectedResolution.width, selectedResolution.height),
            color_r: parseInt(document.getElementById('colorR').value),
            color_g: parseInt(document.getElementById('colorG').value),
            color_b: parseInt(document.getElementById('colorB').value),
            custom_ntp_servers: this.config.custom_ntp_servers || '[]'
        };

        try {
            const response = await fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(config)
            });

            if (response.ok) {
                return true;
            } else {
                throw new Error('Failed to save');
            }
        } catch (e) {
            console.error('Error saving config:', e);
            return false;
        }
    }

    populateForm() {
        // Don't set timezone/region here - let setRegionFromTimezone() handle it
        document.getElementById('ntpServer').value = this.config.ntp_server || 'pool.ntp.org';
        const configWidth = this.config.display_width || BASE_DISPLAY_WIDTH;
        const configHeight = this.config.display_height || BASE_DISPLAY_HEIGHT;

        if (this.displayModes && this.displayModes.length > 0) {
            const resolutionValue = `${configWidth}x${configHeight}`;
            document.getElementById('displayResolution').value = resolutionValue;
            const selectedResolution = this.parseResolutionValue(document.getElementById('displayResolution').value);
            this.populateRefreshRateSelect(selectedResolution.width, selectedResolution.height);
            this.updateTimecodeYBounds(selectedResolution.width, selectedResolution.height);
            this.updateTimecodeXBounds(selectedResolution.width, selectedResolution.height);
        } else {
            this.updateTimecodeYBounds(configWidth, configHeight);
            this.updateTimecodeXBounds(configWidth, configHeight);
        }

        const yInput = document.getElementById('timecodeYOffset');
        yInput.value = this.clampTimecodeYOffset(this.config.timecode_y_offset || 0, configWidth, configHeight);

        const xInput = document.getElementById('timecodeXOffset');
        xInput.value = this.clampTimecodeXOffset(this.config.timecode_x_offset || 0, configWidth, configHeight);

        document.getElementById('colorR').value = this.config.color_r || 64;
        document.getElementById('colorRValue').value = this.config.color_r || 64;
        document.getElementById('colorG').value = this.config.color_g || 255;
        document.getElementById('colorGValue').value = this.config.color_g || 255;
        document.getElementById('colorB').value = this.config.color_b || 64;
        document.getElementById('colorBValue').value = this.config.color_b || 64;

        this.updateColorPreview('colorPreview');
        this.setRegionFromTimezone(this.config.timezone);
    }

    setRegionFromTimezone(tz) {
        if (!this.tzdata.regions || !tz) {
            console.warn('Missing tzdata.regions or tz:', {tz, hasRegions: !!this.tzdata.regions});
            return;
        }

        // Find region containing this timezone
        for (const [region, zones] of Object.entries(this.tzdata.regions)) {
            if (zones.includes(tz)) {
                document.getElementById('region').value = region;
                this.updateTimezoneSelect(region);
                // Give the DOM a moment to update before setting the value
                setTimeout(() => {
                    const tzSelect = document.getElementById('timezone');
                    tzSelect.value = tz;
                }, 10);
                return;
            }
        }
        
        console.warn('Timezone not found in tzdata:', tz, 'Available regions:', Object.keys(this.tzdata.regions));
    }

    populateRegions() {
        if (!this.tzdata.regions) return;

        const regionSelect = document.getElementById('region');
        Object.keys(this.tzdata.regions).forEach(region => {
            const option = document.createElement('option');
            option.value = region;
            option.textContent = region;
            regionSelect.appendChild(option);
        });
    }

    updateTimezoneSelect(region) {
        if (!region || !this.tzdata.regions) {
            console.warn('updateTimezoneSelect: missing region or tzdata', {region, hasRegions: !!this.tzdata.regions});
            document.getElementById('timezone').innerHTML = '<option value="">Select Timezone...</option>';
            return;
        }

        const timezoneSelect = document.getElementById('timezone');
        timezoneSelect.innerHTML = '<option value="">Select Timezone...</option>';

        const zones = this.tzdata.regions[region] || [];
        
        zones.forEach(zone => {
            const option = document.createElement('option');
            option.value = zone;
            option.textContent = zone.replace('_', ' ').split('/').pop();
            timezoneSelect.appendChild(option);
        });
    }
    updateColorPreview(previewId) {
        const preview = document.getElementById(previewId);
        const r = document.getElementById('colorR').value;
        const g = document.getElementById('colorG').value;
        const b = document.getElementById('colorB').value;

        preview.style.backgroundColor = `rgb(${r}, ${g}, ${b})`;
    }

    async handleSubmit(e) {
        e.preventDefault();

        const selectedResolution = this.parseResolutionValue(document.getElementById('displayResolution').value);
        const yInput = document.getElementById('timecodeYOffset');
        const xInput = document.getElementById('timecodeXOffset');
        const clampedY = this.clampTimecodeYOffset(parseInt(yInput.value), selectedResolution.width, selectedResolution.height);
        const clampedX = this.clampTimecodeXOffset(parseInt(xInput.value), selectedResolution.width, selectedResolution.height);
        yInput.value = clampedY;
        xInput.value = clampedX;

        const config = {
            timezone: document.getElementById('timezone').value,
            ntp_server: document.getElementById('ntpServer').value,
            display_width: selectedResolution.width,
            display_height: selectedResolution.height,
            refresh_hz: parseFloat(document.getElementById('refreshHz').value),
            timecode_x_offset: clampedX,
            timecode_y_offset: clampedY,
            color_r: parseInt(document.getElementById('colorR').value),
            color_g: parseInt(document.getElementById('colorG').value),
            color_b: parseInt(document.getElementById('colorB').value),
            custom_ntp_servers: this.config.custom_ntp_servers || '[]'
        };

        this.showStatus('Saving configuration...', 'loading');

        try {
            const response = await fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(config)
            });

            if (response.ok) {
                this.showStatusWithAutoHide('Configuration saved successfully!', 'success');
            } else {
                this.showStatusWithAutoHide('Failed to save configuration', 'error');
            }
        } catch (e) {
            console.error('Error saving config:', e);
            this.showStatusWithAutoHide('Error saving configuration: ' + e.message, 'error');
        }
    }

    resetToDefaults() {
        if (confirm('Reset all settings to defaults?')) {
            document.getElementById('timezone').value = 'Europe/London';
            document.getElementById('ntpServer').value = 'pool.ntp.org';
            document.getElementById('displayResolution').value = `${BASE_DISPLAY_WIDTH}x${BASE_DISPLAY_HEIGHT}`;
            this.populateRefreshRateSelect(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            document.getElementById('refreshHz').value = '50.00';
            this.updateTimecodeYBounds(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            this.updateTimecodeXBounds(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            document.getElementById('timecodeYOffset').value = 0;
            document.getElementById('timecodeXOffset').value = 0;

            document.getElementById('colorR').value = 64;
            document.getElementById('colorRValue').value = 64;
            document.getElementById('colorG').value = 255;
            document.getElementById('colorGValue').value = 255;
            document.getElementById('colorB').value = 64;
            document.getElementById('colorBValue').value = 64;

            this.updateColorPreview('colorPreview');
            this.setRegionFromTimezone('Europe/London');
        }
    }

    showStatus(message, type) {
        const statusEl = document.getElementById('statusMessage');
        statusEl.textContent = message;
        statusEl.className = `status-message ${type}`;
    }

    hideStatus() {
        const statusEl = document.getElementById('statusMessage');
        statusEl.className = 'status-message';
    }

    showStatusWithAutoHide(message, type, duration = 3000) {
        this.showStatus(message, type);
        setTimeout(() => this.hideStatus(), duration);
    }

    async fetchNtpStatus() {
        try {
            const response = await fetch('/api/ntp-status');
            const data = await response.json();
            this.updateNtpStatusDisplay(data);
        } catch (e) {
            console.error('Failed to fetch NTP status:', e);
            this.updateNtpStatusDisplay({ status: 'error', error: 'Connection failed' });
        }
    }

    updateNtpStatusDisplay(data) {
        const indicator = document.getElementById('ntpStatusIndicator');
        const serverEl = document.getElementById('ntpStatusServer');
        const ipEl = document.getElementById('ntpStatusIP');
        const offsetEl = document.getElementById('ntpStatusOffset');
        const textEl = document.getElementById('ntpStatusText');

        // Update indicator class and status text
        indicator.className = 'status-indicator ' + (data.status || 'unknown');
        
        if (data.status === 'synced') {
            textEl.textContent = 'Synced';
        } else if (data.status === 'error') {
            textEl.textContent = 'Error: ' + (data.error || 'Unknown error');
        } else if (data.status === 'syncing') {
            textEl.textContent = 'Syncing...';
        } else {
            textEl.textContent = 'Unknown';
        }

        // Update server info
        serverEl.textContent = data.server || '—';
        ipEl.textContent = data.server_ip || '—';
        offsetEl.textContent = data.offset || '—';
    }

    startNtpStatusRefresh() {
        // Fetch immediately
        this.fetchNtpStatus();
        
        // Refresh every 10 seconds
        setInterval(() => this.fetchNtpStatus(), 10000);
    }
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', () => {
    new ConfigManager();
});
