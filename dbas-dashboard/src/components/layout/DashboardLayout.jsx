import TopNav from './TopNav';

export default function DashboardLayout({ children }) {
  return (
    <div className="min-h-screen flex flex-col">
      <TopNav />
      <main className="flex-1 max-w-[1600px] w-full mx-auto px-6 py-6">{children}</main>
      <footer className="text-center text-[10px] text-[var(--color-text-muted)] py-4">
        DBAS &middot; Driver Behaviour Analysis System &middot; Live fleet telemetry
      </footer>
    </div>
  );
}
