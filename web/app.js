const BASE_DISPLAY_WIDTH = 1920;
const BASE_DISPLAY_HEIGHT = 480;
const BASE_TIME_STRING_HEIGHT = 256;

class ConfigManager {
    constructor() {
        this.config = {};
        this.tzdata = {};
        this.networkStatusWs = null;
        this.wsReconnectDelay = 1500;
        this.configRevision = 0;
        this.isRemoteConfigSyncing = false;
        this.livePositionUpdateTimer = null;
        this.livePositionUpdateInFlight = false;
        this.livePositionUpdateQueued = false;
        this.init();
    }

    setSaveLock(locked, buttonText = null) {
        const saveBtn = document.getElementById('saveBtn');
        if (!saveBtn) {
            return;
        }

        if (!saveBtn.dataset.defaultText) {
            saveBtn.dataset.defaultText = saveBtn.textContent;
        }

        saveBtn.disabled = locked;
        saveBtn.textContent = buttonText || saveBtn.dataset.defaultText;
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
        const scale = this.getGlyphScale(width, height);
        const scaledDigitWidth = Math.max(1, Math.round(209 * scale));
        const scaledTimecodeWidth = 6 * scaledDigitWidth;
        const centerX = (width > scaledTimecodeWidth) ? Math.floor((width - scaledTimecodeWidth) / 2) : 0;

        const minTextX = scaledDigitWidth;
        const maxTextX = width - (7 * scaledDigitWidth);

        const maxLeft = centerX - minTextX;
        const maxRight = maxTextX - centerX;
        return Math.max(0, Math.floor(Math.min(maxLeft, maxRight)));
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
        const ySlider = document.getElementById('timecodeYOffsetSlider');
        const maxOffset = this.getMaxYOffsetForResolution(width, height);
        const maxValue = String(Math.floor(maxOffset));
        const minValue = String(-Math.floor(maxOffset));
        const clamped = this.clampTimecodeYOffset(parseInt(yInput.value), width, height);

        yInput.max = maxValue;
        yInput.min = minValue;
        yInput.value = clamped;

        if (ySlider) {
            ySlider.max = maxValue;
            ySlider.min = minValue;
            ySlider.value = clamped;
        }
    }

    updateTimecodeXBounds(width, height) {
        const xInput = document.getElementById('timecodeXOffset');
        const xSlider = document.getElementById('timecodeXOffsetSlider');
        const maxOffset = this.getMaxXOffsetForResolution(width, height);
        const maxValue = String(Math.floor(maxOffset));
        const minValue = String(-Math.floor(maxOffset));
        const clamped = this.clampTimecodeXOffset(parseInt(xInput.value), width, height);

        xInput.max = maxValue;
        xInput.min = minValue;
        xInput.value = clamped;

        if (xSlider) {
            xSlider.max = maxValue;
            xSlider.min = minValue;
            xSlider.value = clamped;
        }
    }

    getSelectedResolution() {
        return this.parseResolutionValue(document.getElementById('displayResolution').value);
    }

    syncOffsetInputs(axis, rawValue) {
        const selectedResolution = this.getSelectedResolution();
        const isY = axis === 'y';
        const clamped = isY
            ? this.clampTimecodeYOffset(parseInt(rawValue), selectedResolution.width, selectedResolution.height)
            : this.clampTimecodeXOffset(parseInt(rawValue), selectedResolution.width, selectedResolution.height);

        if (isY) {
            document.getElementById('timecodeYOffset').value = clamped;
            document.getElementById('timecodeYOffsetSlider').value = clamped;
        } else {
            document.getElementById('timecodeXOffset').value = clamped;
            document.getElementById('timecodeXOffsetSlider').value = clamped;
        }
    }

    queueLivePositionUpdate() {
        if (this.isRemoteConfigSyncing) {
            return;
        }

        if (this.livePositionUpdateTimer) {
            clearTimeout(this.livePositionUpdateTimer);
        }

        this.livePositionUpdateTimer = setTimeout(() => {
            this.livePositionUpdateTimer = null;
            this.applyLivePositionUpdate();
        }, 120);
    }

    async applyLivePositionUpdate() {
        if (this.livePositionUpdateInFlight) {
            this.livePositionUpdateQueued = true;
            return;
        }

        this.livePositionUpdateInFlight = true;

        try {
            const config = this.buildConfigPayload(true);
            const response = await fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(config)
            });

            const responseData = await response.json().catch(() => ({}));

            if (response.ok) {
                if (responseData.config_revision) {
                    this.configRevision = responseData.config_revision;
                }
                this.config.timecode_x_offset = config.timecode_x_offset;
                this.config.timecode_y_offset = config.timecode_y_offset;
            } else if (response.status === 409) {
                this.isRemoteConfigSyncing = true;
                this.setSaveLock(true, 'Syncing...');
                await this.loadConfig();
            }
        } catch (e) {
            console.error('Live position update failed:', e);
        } finally {
            this.livePositionUpdateInFlight = false;
            if (this.livePositionUpdateQueued) {
                this.livePositionUpdateQueued = false;
                this.applyLivePositionUpdate();
            }
        }
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
            this.queueLivePositionUpdate();
        });

        const yInput = document.getElementById('timecodeYOffset');
        const xInput = document.getElementById('timecodeXOffset');
        const ySlider = document.getElementById('timecodeYOffsetSlider');
        const xSlider = document.getElementById('timecodeXOffsetSlider');

        yInput.addEventListener('input', (e) => {
            this.syncOffsetInputs('y', e.target.value);
            this.queueLivePositionUpdate();
        });
        ySlider.addEventListener('input', (e) => {
            this.syncOffsetInputs('y', e.target.value);
            this.queueLivePositionUpdate();
        });

        xInput.addEventListener('input', (e) => {
            this.syncOffsetInputs('x', e.target.value);
            this.queueLivePositionUpdate();
        });
        xSlider.addEventListener('input', (e) => {
            this.syncOffsetInputs('x', e.target.value);
            this.queueLivePositionUpdate();
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

        // VLAN enable checkbox
        document.getElementById('adminVlanEnabled').addEventListener('change', () => {
            this.toggleVlanFields();
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
            this.configRevision = this.config.config_revision || this.configRevision;
            this.populateForm();
            if (this.isRemoteConfigSyncing) {
                this.isRemoteConfigSyncing = false;
                this.setSaveLock(false);
            }
        } catch (e) {
            console.error('Failed to load config:', e);
            if (this.isRemoteConfigSyncing) {
                this.isRemoteConfigSyncing = false;
                this.setSaveLock(false);
            }
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
                { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT, refresh: 60.00, preferred: true },
                { width: BASE_DISPLAY_WIDTH, height: BASE_DISPLAY_HEIGHT, refresh: 59.94, preferred: false }
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
        const config = this.buildConfigPayload(false);

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
        const clampedY = this.clampTimecodeYOffset(this.config.timecode_y_offset || 0, configWidth, configHeight);
        yInput.value = clampedY;
        const ySlider = document.getElementById('timecodeYOffsetSlider');
        if (ySlider) {
            ySlider.value = clampedY;
        }

        const xInput = document.getElementById('timecodeXOffset');
        const clampedX = this.clampTimecodeXOffset(this.config.timecode_x_offset || 0, configWidth, configHeight);
        xInput.value = clampedX;
        const xSlider = document.getElementById('timecodeXOffsetSlider');
        if (xSlider) {
            xSlider.value = clampedX;
        }
        document.getElementById('transitionFadeMs').value = this.config.transition_fade_ms ?? 120;

        document.getElementById('colorR').value = this.config.color_r || 64;
        document.getElementById('colorRValue').value = this.config.color_r || 64;
        document.getElementById('colorG').value = this.config.color_g || 255;
        document.getElementById('colorGValue').value = this.config.color_g || 255;
        document.getElementById('colorB').value = this.config.color_b || 64;
        document.getElementById('colorBValue').value = this.config.color_b || 64;

        this.updateColorPreview('colorPreview');
        this.setRegionFromTimezone(this.config.timezone);
        
        // Network settings
        document.getElementById('adminVlanEnabled').checked = this.config.admin_vlan_enabled || false;
        document.getElementById('adminVlanIp').value = this.config.admin_vlan_ip || '192.168.1.100/24';
        document.getElementById('adminVlanGateway').value = this.config.admin_vlan_gateway || '';
        document.getElementById('webUiBindAddress').value = this.config.web_ui_bind_address || '0.0.0.0';
        document.getElementById('webUiBindPort').value = this.config.web_ui_bind_port || 8080;
        this.toggleVlanFields();
        this.startNetworkStatusRefresh();
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

    toggleVlanFields() {
        const enabled = document.getElementById('adminVlanEnabled').checked;
        const vlanConfigGroup = document.getElementById('vlanConfigGroup');
        const vlanGatewayGroup = document.getElementById('vlanGatewayGroup');
        
        vlanConfigGroup.style.display = enabled ? 'block' : 'none';
        vlanGatewayGroup.style.display = enabled ? 'block' : 'none';
        
        // Update requiredness
        document.getElementById('adminVlanIp').required = enabled;
    }

    startNetworkStatusRefresh() {
        this.ensureNetworkStatusTable();
        if (!this.networkStatusWs) {
            this.connectNetworkStatusWs();
        }
    }

    ensureNetworkStatusTable() {
        const content = document.getElementById('networkStatusContent');
        if (content.querySelector('.interfaces-table')) {
            return;
        }

        let html = '<table class="interfaces-table">';
        html += '<thead><tr>';
        html += '<th>Interface</th>';
        html += '<th class="bandwidth-col">Tx</th>';
        html += '<th class="bandwidth-col">Rx</th>';
        html += '</tr></thead><tbody>';
        html += '</tbody></table>';
        content.innerHTML = html;
    }

    connectNetworkStatusWs() {
        try {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const currentPort = parseInt(window.location.port) || (window.location.protocol === 'https:' ? 443 : 80);
            const wsPort = currentPort + 1;
            const wsUrl = `${protocol}//${window.location.hostname}:${wsPort}`;
            
            this.networkStatusWs = new WebSocket(wsUrl);
            this.networkStatusWs.onopen = () => {
                console.log('Network status WebSocket connected');
            };
            this.networkStatusWs.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    if (data.type === 'config_update') {
                        const incomingRevision = data.config_revision || 0;

                        if (this.livePositionUpdateInFlight || this.livePositionUpdateTimer) {
                            if (incomingRevision > this.configRevision) {
                                this.configRevision = incomingRevision;
                            }
                            return;
                        }

                        if (incomingRevision > this.configRevision) {
                            this.isRemoteConfigSyncing = true;
                            this.setSaveLock(true, 'Syncing...');
                            this.showStatusWithAutoHide('Configuration updated in another browser, refreshing...', 'warning', 3500);
                            this.loadConfig();
                        }
                        return;
                    }
                    if (data.interfaces) {
                        this.updateNetworkStatusTableWithRates(data);
                    }
                } catch (e) {
                    console.error('Failed to parse network status message:', e);
                }
            };
            this.networkStatusWs.onerror = (error) => {
                console.error('Network status WebSocket error:', error);
                this.networkStatusWs = null;
            };
            this.networkStatusWs.onclose = () => {
                console.log('Network status WebSocket closed');
                this.networkStatusWs = null;
                setTimeout(() => this.connectNetworkStatusWs(), this.wsReconnectDelay);
            };
        } catch (e) {
            console.error('WebSocket connection failed:', e);
            this.networkStatusWs = null;
            setTimeout(() => this.connectNetworkStatusWs(), this.wsReconnectDelay);
        }
    }

    stopNetworkStatusRefresh() {
        if (this.networkStatusWs) {
            this.networkStatusWs.close();
            this.networkStatusWs = null;
        }
    }

    updateNetworkStatusTableWithRates(data) {
        if (!data.interfaces || data.interfaces.length === 0) {
            document.getElementById('networkStatusContent').innerHTML = '<p>No network interfaces found</p>';
            return;
        }
        
        const content = document.getElementById('networkStatusContent');
        
        // If table doesn't exist, create initial table with bandwidth columns
        if (!content.querySelector('.interfaces-table')) {
            this.ensureNetworkStatusTable();
        }
        
        // Update or create table rows
        const table = content.querySelector('.interfaces-table tbody');
        if (!table) return;
        
        data.interfaces.forEach(iface => {
            let row = table.querySelector(`tr[data-interface="${iface.name}"]`);
            
            if (!row) {
                // Create new row for new interface
                row = document.createElement('tr');
                row.className = 'interface-row';
                row.setAttribute('data-interface', iface.name);
                row.setAttribute('data-type', iface.type || 'ethernet');
                
                const typeIcon = this.getInterfaceIcon(iface.type || 'ethernet');
                row.innerHTML = `
                    <td class="interface-name">${typeIcon} ${iface.name}</td>
                    <td class="bandwidth-tx">${this.formatBandwidth(iface.tx_rate_kbps || 0)}</td>
                    <td class="bandwidth-rx">${this.formatBandwidth(iface.rx_rate_kbps || 0)}</td>
                `;
                table.appendChild(row);
            } else {
                // Update existing row
                const cells = row.querySelectorAll('td');
                if (cells.length >= 3) {
                    cells[1].textContent = this.formatBandwidth(iface.tx_rate_kbps || 0);
                    cells[2].textContent = this.formatBandwidth(iface.rx_rate_kbps || 0);
                }
            }
        });
    }

    getInterfaceIcon(type) {
        switch(type) {
            case 'ethernet': return '🔌';
            case 'wifi': return '📶';
            case 'vlan': return '🏷️';
            default: return '🔗';
        }
    }

    formatBandwidth(kbps) {
        if (!kbps || kbps === 0) return '0 kbps';
        if (kbps >= 1000) {
            return (kbps / 1000).toFixed(2) + ' Mbps';
        }
        return kbps.toFixed(1) + ' kbps';
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

        if (this.isRemoteConfigSyncing) {
            this.showStatusWithAutoHide('Sync in progress, please wait...', 'warning', 2000);
            return;
        }

        this.setSaveLock(true, 'Saving...');

        const selectedResolution = this.getSelectedResolution();
        const yInput = document.getElementById('timecodeYOffset');
        const xInput = document.getElementById('timecodeXOffset');
        const clampedY = this.clampTimecodeYOffset(parseInt(yInput.value), selectedResolution.width, selectedResolution.height);
        const clampedX = this.clampTimecodeXOffset(parseInt(xInput.value), selectedResolution.width, selectedResolution.height);
        yInput.value = clampedY;
        xInput.value = clampedX;
        document.getElementById('timecodeYOffsetSlider').value = clampedY;
        document.getElementById('timecodeXOffsetSlider').value = clampedX;

        const config = this.buildConfigPayload(true);

        this.showStatus('Saving configuration...', 'loading');

        try {
            const response = await fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(config)
            });

            const responseData = await response.json().catch(() => ({}));

            if (response.ok) {
                if (responseData.config_revision) {
                    this.configRevision = responseData.config_revision;
                }
                // Apply network VLAN configuration
                try {
                    const vlanResponse = await fetch('/api/network/vlan', {
                        method: 'POST',
                        headers: {
                            'Content-Type': 'application/json'
                        },
                        body: JSON.stringify(config)
                    });
                    
                    if (vlanResponse.ok) {
                        this.showStatusWithAutoHide('Configuration saved and network applied successfully!', 'success');
                    } else {
                        this.showStatusWithAutoHide('Configuration saved but network changes failed', 'warning');
                    }
                } catch (netErr) {
                    console.error('Error applying network config:', netErr);
                    this.showStatusWithAutoHide('Configuration saved but network changes failed', 'warning');
                }
            } else if (response.status === 409) {
                this.showStatusWithAutoHide('Configuration changed in another browser. Reloading latest values...', 'warning', 4000);
                this.isRemoteConfigSyncing = true;
                this.setSaveLock(true, 'Syncing...');
                await this.loadConfig();
            } else {
                this.showStatusWithAutoHide('Failed to save configuration', 'error');
            }
        } catch (e) {
            console.error('Error saving config:', e);
            this.showStatusWithAutoHide('Error saving configuration: ' + e.message, 'error');
        } finally {
            if (!this.isRemoteConfigSyncing) {
                this.setSaveLock(false);
            }
        }
    }

    resetToDefaults() {
        if (confirm('Reset all settings to defaults?')) {
            document.getElementById('timezone').value = 'Europe/London';
            document.getElementById('ntpServer').value = 'pool.ntp.org';
            document.getElementById('displayResolution').value = `${BASE_DISPLAY_WIDTH}x${BASE_DISPLAY_HEIGHT}`;
            this.populateRefreshRateSelect(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            document.getElementById('refreshHz').value = '60.00';
            this.updateTimecodeYBounds(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            this.updateTimecodeXBounds(BASE_DISPLAY_WIDTH, BASE_DISPLAY_HEIGHT);
            document.getElementById('timecodeYOffset').value = 0;
            document.getElementById('timecodeXOffset').value = 0;
            document.getElementById('timecodeYOffsetSlider').value = 0;
            document.getElementById('timecodeXOffsetSlider').value = 0;
            document.getElementById('transitionFadeMs').value = 120;

            document.getElementById('colorR').value = 64;
            document.getElementById('colorRValue').value = 64;
            document.getElementById('colorG').value = 255;
            document.getElementById('colorGValue').value = 255;
            document.getElementById('colorB').value = 64;
            document.getElementById('colorBValue').value = 64;

            document.getElementById('adminVlanEnabled').checked = false;
            document.getElementById('adminVlanIp').value = '192.168.1.100/24';
            document.getElementById('adminVlanGateway').value = '';
            document.getElementById('webUiBindAddress').value = '0.0.0.0';
            document.getElementById('webUiBindPort').value = '8080';
            this.toggleVlanFields();

            this.updateColorPreview('colorPreview');
            this.setRegionFromTimezone('Europe/London');
        }
    }

    buildConfigPayload(withRevision) {
        const selectedResolution = this.getSelectedResolution();
        const yInput = document.getElementById('timecodeYOffset');
        const xInput = document.getElementById('timecodeXOffset');
        const timezoneValue = document.getElementById('timezone').value || this.config.timezone || 'Europe/London';
        const ntpValue = document.getElementById('ntpServer').value || this.config.ntp_server || 'pool.ntp.org';
        const clampedY = this.clampTimecodeYOffset(parseInt(yInput.value), selectedResolution.width, selectedResolution.height);
        const clampedX = this.clampTimecodeXOffset(parseInt(xInput.value), selectedResolution.width, selectedResolution.height);

        yInput.value = clampedY;
        xInput.value = clampedX;

        const ySlider = document.getElementById('timecodeYOffsetSlider');
        const xSlider = document.getElementById('timecodeXOffsetSlider');
        if (ySlider) {
            ySlider.value = clampedY;
        }
        if (xSlider) {
            xSlider.value = clampedX;
        }

        const config = {
            timezone: timezoneValue,
            ntp_server: ntpValue,
            display_width: selectedResolution.width,
            display_height: selectedResolution.height,
            refresh_hz: parseFloat(document.getElementById('refreshHz').value),
            timecode_x_offset: clampedX,
            timecode_y_offset: clampedY,
            transition_fade_ms: parseInt(document.getElementById('transitionFadeMs').value) || 0,
            color_r: parseInt(document.getElementById('colorR').value),
            color_g: parseInt(document.getElementById('colorG').value),
            color_b: parseInt(document.getElementById('colorB').value),
            custom_ntp_servers: this.config.custom_ntp_servers || '[]',
            admin_vlan_enabled: document.getElementById('adminVlanEnabled').checked ? 1 : 0,
            admin_vlan_ip: document.getElementById('adminVlanIp').value || '192.168.1.100/24',
            admin_vlan_gateway: document.getElementById('adminVlanGateway').value || '',
            web_ui_bind_address: document.getElementById('webUiBindAddress').value || '0.0.0.0',
            web_ui_bind_port: parseInt(document.getElementById('webUiBindPort').value) || 8080
        };

        if (withRevision) {
            config.config_revision = this.configRevision;
        }

        return config;
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
