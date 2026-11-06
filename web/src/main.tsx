import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import './styles/global.css';
import { Storefront } from './storefront/Storefront';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <Storefront />
  </StrictMode>,
);
