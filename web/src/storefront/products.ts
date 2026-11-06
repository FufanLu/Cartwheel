// Demo catalog. Synthetic data until deal-svc owns the catalog (M4).
export interface Product {
  entry: string;
  sku: string;
  title: string;
  spec: string;
  unit_price_cents: number;
  plate: PlateId;
}

export type PlateId = 'kettle' | 'mug' | 'notebook' | 'lamp' | 'headphones' | 'tote';

export const PRODUCTS: Product[] = [
  { entry: '01', sku: 'CW-1001', title: 'Stovetop kettle', spec: 'Brushed steel · 1.2 L', unit_price_cents: 3400, plate: 'kettle' },
  { entry: '02', sku: 'CW-1002', title: 'Stoneware mug', spec: 'Glazed · 350 ml', unit_price_cents: 1400, plate: 'mug' },
  { entry: '03', sku: 'CW-1003', title: 'Grid notebook', spec: 'A5 · 5 mm squares · 160 pp', unit_price_cents: 950, plate: 'notebook' },
  { entry: '04', sku: 'CW-1004', title: 'Articulated desk lamp', spec: 'Two arms · warm LED', unit_price_cents: 5800, plate: 'lamp' },
  { entry: '05', sku: 'CW-1005', title: 'Wired headphones', spec: 'Closed back · 1.5 m cord', unit_price_cents: 4200, plate: 'headphones' },
  { entry: '06', sku: 'CW-1006', title: 'Canvas tote', spec: '12 oz cotton duck', unit_price_cents: 1900, plate: 'tote' },
];
