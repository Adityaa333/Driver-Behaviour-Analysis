import { createContext, useContext } from 'react';
import useDeviceDiscovery from '../hooks/useDeviceDiscovery';

const DeviceContext = createContext(null);

export function DeviceProvider({ children }) {
  const deviceState = useDeviceDiscovery();
  return <DeviceContext.Provider value={deviceState}>{children}</DeviceContext.Provider>;
}

/** Access the fleet's device list + currently-selected device id/setter. */
export function useDeviceContext() {
  const ctx = useContext(DeviceContext);
  if (!ctx) {
    throw new Error('useDeviceContext must be used within a DeviceProvider');
  }
  return ctx;
}
