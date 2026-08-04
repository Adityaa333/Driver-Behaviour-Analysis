import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { DeviceProvider } from './context/DeviceContext';
import DashboardLayout from './components/layout/DashboardLayout';
import DashboardPage from './pages/DashboardPage';

export default function App() {
  return (
    <DeviceProvider>
      <BrowserRouter>
        <DashboardLayout>
          <Routes>
            <Route path="/" element={<DashboardPage />} />
            <Route path="*" element={<DashboardPage />} />
          </Routes>
        </DashboardLayout>
      </BrowserRouter>
    </DeviceProvider>
  );
}
