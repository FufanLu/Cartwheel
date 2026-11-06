import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import './styles/global.css';
import { Storefront } from './storefront/Storefront';
import { OpsConsole } from './ops/OpsConsole';

// Two sheets of one pad. No router library: the path picks the sheet.
const onOps = window.location.pathname.replace(/\/+$/, '') === '/ops';

createRoot(document.getElementById('root')!).render(
  <StrictMode>{onOps ? <OpsConsole /> : <Storefront />}</StrictMode>,
);
