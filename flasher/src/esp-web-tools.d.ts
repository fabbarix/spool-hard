// esp-web-tools ships its own .d.ts but doesn't register the
// <esp-web-install-button> custom element with React's JSX namespace.
// React 19 reads intrinsic elements off `React.JSX`, so the augmentation
// has to live there (not the global `JSX` namespace, which used to work
// pre-19 and silently no-ops in React 19).

declare module 'esp-web-tools';

declare namespace React {
  namespace JSX {
    interface IntrinsicElements {
      'esp-web-install-button': React.DetailedHTMLProps<
        React.HTMLAttributes<HTMLElement> & {
          manifest?: string;
          'erase-first'?: boolean | string;
        },
        HTMLElement
      >;
    }
  }
}
