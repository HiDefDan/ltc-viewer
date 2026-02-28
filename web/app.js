console.log('app.js loaded - hello world from app.js');

const DISPLAY_HEIGHT = 1080;
const TIME_STRING_HEIGHT = 256;
const MAX_TIMECODE_Y = DISPLAY_HEIGHT - TIME_STRING_HEIGHT;

class ConfigManager {
    constructor() {
        this.config = {};
        this.tzdata = {};
        this.init();
    }

    clampTimecodeY(value) {
        if (Number.isNaN(value)) return 0;
        return Math.min(MAX_TIMECODE_Y, Math.max(0, value));
    }

    async init() {
        console.log('ConfigManager.init() starting...');
        this.setupEventListeners();
        await this.loadTimezoneData();
        console.log('Timezone data loaded, regions:', Object.keys(this.tzdata.regions || {}));
        this.populateRegions();
        console.log('Regions populated in dropdown');
        await this.loadConfig();
        console.log('Config loaded and form populated');
        await this.loadNtpServers();
        console.log('NTP servers loaded with custom servers');
        await this.loadDisplayModes();
        console.log('Display modes loaded from hardware');
        this.startNtpStatusRefresh();
        console.log('NTP status refresh started');
    }

    setupEventListeners() {
        const form = document.getElementById('configForm');
        form.addEventListener('submit', (e) => this.handleSubmit(e));

        document.getElementById('resetBtn').addEventListener('click', () => this.resetToDefaults());

        // Region selector
        document.getElementById('region').addEventListener('change', (e) => {
            this.updateTimezoneSelect(e.target.value);
        });

        // Color sliders
        ['colorR', 'colorG', 'colorB'].forEach(id => {
            document.getElementById(id).addEventListener('input', (e) => {
                document.getElementById(id + 'Value').value = e.target.value;
                this.updateColorPreview('colorPreview');
            });
        });

        // NTP server selection
        document.getElementById('ntpServer').addEventListener('change', (e) => {
            console.log('NTP Server selected:', e.target.value);
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
                this.populateRefreshRateSelect();
            } else {
                console.error('No display modes returned from API');
            }
        } catch (e) {
            console.error('Failed to load display modes:', e);
            // Fallback to default modes
            this.displayModes = [50, 60];
            this.populateRefreshRateSelect();
        }
    }

    populateRefreshRateSelect() {
        const select = document.getElementById('refreshHz');
        select.innerHTML = '';

        this.displayModes.forEach(mode => {
            const option = document.createElement('option');
            option.value = mode;
            
            // Format with appropriate label
            if (mode === 59.94) {
                option.textContent = `${mode} Hz (NTSC)`;
            } else if (mode === 50.00 || mode === 50) {
                option.textContent = `${mode} Hz (PAL)`;
            } else if (mode === 60.00 || mode === 60) {
                option.textContent = `${mode} Hz`;
            } else {
                option.textContent = `${mode} Hz`;
            }
            
            select.appendChild(option);
        });

        // Set current value if config is loaded
        if (this.config && this.config.refresh_hz) {
            select.value = this.config.refresh_hz;
        }
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
        const config = {
            timezone: document.getElementById('timezone').value,
            ntp_server: document.getElementById('ntpServer').value,
            refresh_hz: parseFloat(document.getElementById('refreshHz').value),
            timecode_y: parseInt(document.getElementById('timecodeY').value),
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
        
        // Set refresh rate - match config value to closest available mode
        const configHz = this.config.refresh_hz || 50;
        const select = document.getElementById('refreshHz');
        let bestMatch = null;
        let minDiff = Infinity;
        
        // Find closest match in available modes
        for (let i = 0; i < select.options.length; i++) {
            const optionValue = parseFloat(select.options[i].value);
            const diff = Math.abs(optionValue - configHz);
            if (diff < minDiff) {
                minDiff = diff;
                bestMatch = select.options[i].value;
            }
        }
        
        if (bestMatch !== null) {
            select.value = bestMatch;
        }
        
        const yInput = document.getElementById('timecodeY');
        yInput.max = String(MAX_TIMECODE_Y);
        yInput.value = this.clampTimecodeY(this.config.timecode_y || 412);

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

        console.log('setRegionFromTimezone called with:', tz);

        // Find region containing this timezone
        for (const [region, zones] of Object.entries(this.tzdata.regions)) {
            if (zones.includes(tz)) {
                console.log('Found region:', region, 'for timezone:', tz);
                document.getElementById('region').value = region;
                this.updateTimezoneSelect(region);
                // Give the DOM a moment to update before setting the value
                setTimeout(() => {
                    const tzSelect = document.getElementById('timezone');
                    tzSelect.value = tz;
                    console.log('Set timezone value to:', tz, 'actual value:', tzSelect.value);
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
        console.log('updateTimezoneSelect: region:', region, 'zones:', zones);
        
        zones.forEach(zone => {
            const option = document.createElement('option');
            option.value = zone;
            option.textContent = zone.replace('_', ' ').split('/').pop();
            timezoneSelect.appendChild(option);
        });
        
        console.log('updateTimezoneSelect: created', zones.length, 'options');
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

        const yInput = document.getElementById('timecodeY');
        const clampedY = this.clampTimecodeY(parseInt(yInput.value));
        yInput.value = clampedY;

        const config = {
            timezone: document.getElementById('timezone').value,
            ntp_server: document.getElementById('ntpServer').value,
            refresh_hz: parseFloat(document.getElementById('refreshHz').value),
            timecode_y: clampedY,
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
            document.getElementById('refreshHz').value = 50;
            document.getElementById('timecodeY').value = 412;

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
