// Main App component
const App = () => {
    const [connected, setConnected] = React.useState(false);
    const [loading, setLoading] = React.useState(true);
    const [error, setError] = React.useState(null);
    const [lastMessageTime, setLastMessageTime] = React.useState(0);
    const [systemInfo, setSystemInfo] = React.useState({
        freeHeap: 0,
        socTemp: 0
    });
    const [settings, setSettings] = React.useState({
        deviceInfo: {
            name: 'TBD',
            firmwareVersion: 'TBD',
            hardwareVersion: 'TBD',
            macAddress: '00:00:00:00:00:00'
        },
        power: {
            sleepTimeout: 30,
            lowPowerMode: false,
            enableSleep: true,
            deepSleep: false
        },
        led: {
            brightness: 80
        },
        connectivity: {
            bleTxPower: 'low',
            bleReconnectDelay: 3
        }
    });
    const [statusMessage, setStatusMessage] = React.useState(null);
    const [statusType, setStatusType] = React.useState('info');
    const [otaProgress, setOtaProgress] = React.useState(0);
    const [otaInProgress, setOtaInProgress] = React.useState(false);
    const socketRef = React.useRef(null);
    const wsCheckIntervalRef = React.useRef(null);
    const fileInputRef = React.useRef(null);
    const initialSettingsRef = React.useRef(null);

    React.useEffect(() => {
        connectWebSocket();
        wsCheckIntervalRef.current = setInterval(checkWebSocketActivity, 1000);
        return () => {
            if (socketRef.current) {
                socketRef.current.close();
            }
            if (wsCheckIntervalRef.current) {
                clearInterval(wsCheckIntervalRef.current);
            }
        };
    }, []);

    const checkWebSocketActivity = () => {
        const now = Date.now();
        if (lastMessageTime > 0 && now - lastMessageTime > 1500) {
            setConnected(false);
            setLoading(true);
            if (socketRef.current && socketRef.current.readyState !== WebSocket.CLOSED) {
                try {
                    socketRef.current.close();
                } catch (e) {
                    console.error('Error closing socket:', e);
                }
            }
            setTimeout(connectWebSocket, 1000);
        }
    };

    const connectWebSocket = () => {
        setLoading(true);
        setError(null);

        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}/ws`;

        const socket = new WebSocket(wsUrl);
        socketRef.current = socket;

        socket.onopen = () => {
            console.log('WebSocket connected');
            setConnected(true);
            setLoading(false);
            setLastMessageTime(Date.now());
            requestSettings();
        };

        socket.onclose = () => {
            console.log('WebSocket disconnected');
            setConnected(false);
            setLoading(true);
            setTimeout(connectWebSocket, 1000);
        };

        socket.onerror = (error) => {
            console.error('WebSocket error:', error);
            setError('Failed to connect to the device. Please try again later.');
            setLoading(false);
        };

        socket.onmessage = (event) => {
            handleWebSocketMessage(event.data);
        };
    };

    const handleWebSocketMessage = (data) => {
        setLastMessageTime(Date.now());

        try {
            const message = JSON.parse(data);

            switch (message.type) {
                case 'ota_progress':
                    if (message.content && message.content.progress !== undefined) {
                        const progress = typeof message.content === 'string'
                            ? JSON.parse(message.content).progress
                            : message.content.progress;

                        setOtaProgress(progress);

                        if (progress === 100) {
                            showStatus('OTA update completed successfully! Device will reboot.', 'success');
                            setTimeout(() => {
                                setOtaInProgress(false);
                            }, 2000);
                        }
                    }
                    break;
                case 'settings':
                    if (message.content) {
                        const newSettings = {...settings, ...message.content};
                        setSettings(newSettings);
                        if (!initialSettingsRef.current) {
                            initialSettingsRef.current = JSON.stringify(newSettings);
                        }
                    }
                    break;
                case 'settings_update_status':
                    if (message.content.success) {
                        showStatus('Settings updated successfully. The device is restarting.', 'success');
                    } else {
                        showStatus(`Failed to update settings: ${message.content.error}`, 'error');
                    }
                    break;
                case 'log':
                    console.log('Server log:', message.content);
                    break;
                case 'ping':
                    if (message.content) {
                        try {
                            const pingData = typeof message.content === 'string'
                                ? JSON.parse(message.content)
                                : message.content;

                            setSystemInfo({
                                freeHeap: pingData.freeHeap || 0,
                                socTemp: pingData.socTemp || 0
                            });
                        } catch (e) {
                            console.error('Error parsing ping data:', e);
                        }
                    }
                    break;
                default:
                    console.log('Unknown message type:', message.type);
            }
        } catch (error) {
            console.error('Error parsing WebSocket message:', error);
        }
    };

    const requestSettings = () => {
        if (!socketRef.current || socketRef.current.readyState !== WebSocket.OPEN) {
            showStatus('WebSocket not connected', 'error');
            return;
        }

        socketRef.current.send(JSON.stringify({
            type: 'command',
            command: 'get_settings'
        }));
    };

    const saveSettings = () => {
        if (!socketRef.current || socketRef.current.readyState !== WebSocket.OPEN) {
            showStatus('WebSocket not connected', 'error');
            return;
        }

        socketRef.current.send(JSON.stringify({
            type: 'command',
            command: 'update_settings',
            content: settings
        }));

        showStatus('Saving settings...', 'info');
    };

    const updateSetting = (category, key, value) => {
        setSettings(prevSettings => ({
            ...prevSettings,
            [category]: {
                ...prevSettings[category],
                [key]: value
            }
        }));
    };

    const showStatus = (message, type) => {
        setStatusMessage(message);
        setStatusType(type);
        setTimeout(() => {
            setStatusMessage(null);
        }, 5000);
    };

    if (loading || !connected) {
        return (
            <div id="loadingContainer">
                <div className="spinner"></div>
                <p>{connected ? 'Loading settings' : 'Waiting for connection...'}</p>
            </div>
        );
    }

    if (error) {
        return (
            <div className="container">
                <div className="status error">{error}</div>
                <button onClick={connectWebSocket}>Retry Connection</button>
            </div>
        );
    }

    return (
        <div>
            <h1>Device Settings</h1>

            <div className="container">
                {statusMessage && (
                    <div className={`status ${statusType}`}>
                        {statusMessage}
                    </div>
                )}

                <button onClick={saveSettings} disabled={!connected || otaInProgress || (initialSettingsRef.current && JSON.stringify(settings) === initialSettingsRef.current)}>
                    Save & Reboot
                </button>
                <button onClick={() => window.location.href = '/'} style={{marginLeft: '10px', backgroundColor: '#6c757d'}}
                        disabled={otaInProgress}>
                    Return
                </button>

                <div className="setting-group">
                    <h2>Device Information</h2>

                    <div className="setting-item">
                        <div className="setting-title">Device Name</div>
                        <div>{settings.deviceInfo.name}</div>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Firmware Version</div>
                        <div>{settings.deviceInfo.firmwareVersion}</div>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Hardware Version</div>
                        <div>{settings.deviceInfo.hardwareVersion}</div>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">MAC Address</div>
                        <div>{settings.deviceInfo.macAddress}</div>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Free Heap</div>
                        <div>{(systemInfo.freeHeap / 1000).toFixed(0)} kb</div>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">SoC Temperature</div>
                        <div>{systemInfo.socTemp.toFixed(0)}°C</div>
                    </div>
                </div>

                <div className="setting-group">
                    <h2>Power Management</h2>

                    <div className="setting-item">
                        <div className="setting-title">Low Power Mode</div>
                        <div className="setting-description">
                            Sacrifice performance to improve battery life.
                            Makes BLE interval to be ~12-15ms instead of 7.5ms.
                            Makes RGB LEDs to update with 60FPS instead of 120FPS.
                        </div>
                        <label className="toggle-switch">
                            <input
                                type="checkbox"
                                checked={settings.power.lowPowerMode}
                                onChange={(e) => updateSetting('power', 'lowPowerMode', e.target.checked)}
                            />
                            <span className="slider"></span>
                        </label>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Enable Sleep</div>
                        <div className="setting-description">
                            Enable device to enter sleep mode and disable Bluetooth when no events received in the
                            time window
                        </div>
                        <label className="toggle-switch">
                            <input
                                type="checkbox"
                                checked={settings.power.enableSleep}
                                onChange={(e) => updateSetting('power', 'enableSleep', e.target.checked)}
                            />
                            <span className="slider"></span>
                        </label>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Deep Sleep</div>
                        <div className="setting-description">
                            When enabled, USB device will not be able to wake up the device. You will have to press any button on the device itself.
                        </div>
                        <label className="toggle-switch">
                            <input
                                type="checkbox"
                                checked={settings.power.deepSleep}
                                onChange={(e) => updateSetting('power', 'deepSleep', e.target.checked)}
                            />
                            <span className="slider"></span>
                        </label>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Sleep Timeout</div>
                        <div className="setting-description">
                            Time without USB events in seconds before device enters sleep mode
                        </div>
                        <input
                            type="number"
                            min="0"
                            max="3600"
                            value={settings.power.sleepTimeout}
                            onChange={(e) => updateSetting('power', 'sleepTimeout', parseInt(e.target.value))}
                        />
                    </div>
                </div>

                <div className="setting-group">
                    <h2>LED Configuration</h2>

                    <div className="setting-item">
                        <div className="setting-title">Brightness</div>
                        <div className="setting-description">
                            Global LED brightness percentage
                        </div>
                        <input
                            type="range"
                            min="0"
                            max="100"
                            value={settings.led.brightness}
                            onChange={(e) => updateSetting('led', 'brightness', parseInt(e.target.value))}
                        />
                    </div>
                </div>

                <div className="setting-group">
                    <h2>Connectivity</h2>

                    <div className="setting-item">
                        <div className="setting-title">BLE TX Power</div>
                        <div className="setting-description">
                            Bluetooth transmission power level
                        </div>
                        <select
                            value={settings.connectivity.bleTxPower}
                            onChange={(e) => updateSetting('connectivity', 'bleTxPower', e.target.value)}
                        >
                            <option value="n6">-6 dB</option>
                            <option value="n3">-3 dB</option>
                            <option value="n0">0 dB</option>
                            <option value="p3">+3 dB</option>
                            <option value="p6">+6 dB</option>
                            <option value="p9">+9 dB</option>
                        </select>
                    </div>

                    <div className="setting-item">
                        <div className="setting-title">Reconnect Delay</div>
                        <div className="setting-description">
                            Delay in seconds before attempting to reconnect after Bluetooth disconnect (doesn't
                            affect wake from sleep)
                        </div>
                        <input
                            type="number"
                            min="1"
                            max="60"
                            value={settings.connectivity.bleReconnectDelay}
                            onChange={(e) => updateSetting('connectivity', 'bleReconnectDelay', parseInt(e.target.value))}
                        />
                    </div>
                </div>

                <div className="setting-group">
                    <h2>Firmware Update</h2>

                    <div className="setting-item">
                        <div className="setting-title">OTA Update</div>
                        <div className="setting-description">
                            Upload a new firmware file to update the device. The device will reboot after a
                            successful update.
                        </div>

                        {otaInProgress ? (
                            <div>
                                <div className="progress-container">
                                    <div
                                        className="progress-bar"
                                        style={{width: `${otaProgress}%`}}
                                    >
                                        {otaProgress}%
                                    </div>
                                </div>
                                <p>Uploading firmware... Please do not disconnect the device.</p>
                            </div>
                        ) : (
                            <div>
                                <div className="file-input-container">
                                    <input
                                        type="file"
                                        ref={fileInputRef}
                                        accept=".bin"
                                    />
                                </div>
                                <button
                                    onClick={() => {
                                        const fileInput = fileInputRef.current;
                                        if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
                                            showStatus('Please select a firmware file', 'error');
                                            return;
                                        }

                                        const file = fileInput.files[0];
                                        const formData = new FormData();
                                        formData.append('firmware', file);

                                        setOtaInProgress(true);
                                        setOtaProgress(0);
                                        showStatus('Starting firmware upload...', 'info');
                                        window.scrollTo(0, 0);

                                        fetch('/upload', {
                                            method: 'POST',
                                            body: formData
                                        })
                                            .then(response => {
                                                if (!response.ok) {
                                                    throw new Error(`HTTP error ${response.status}`);
                                                }
                                                return response.text();
                                            })
                                            .then(data => {
                                                console.log('Upload successful:', data);
                                            })
                                            .catch(error => {
                                                console.error('Upload failed:', error);
                                                setOtaInProgress(false);
                                                showStatus(`Upload failed: ${error.message}`, 'error');
                                            });
                                    }}
                                    disabled={!connected || !fileInputRef.current?.files?.length}
                                >
                                    Upload Firmware
                                </button>
                            </div>
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
};

ReactDOM.render(<App/>, document.getElementById('root'));
