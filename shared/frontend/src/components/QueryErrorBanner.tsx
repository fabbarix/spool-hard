import { useEffect, useState } from 'react';
import { useQueryClient, type Query } from '@tanstack/react-query';
import { ErrorBanner } from './ErrorBanner';

const isFailedActive = (q: Query) => q.state.status === 'error' && q.getObserversCount() > 0;

/**
 * Banner shown while any currently-mounted query is in the error state
 * (e.g. the device is unreachable mid-reboot). Retry refetches exactly the
 * failed queries. Renders nothing when everything is healthy.
 */
export function QueryErrorBanner({ title = 'Could not load some settings' }: { title?: string }) {
  const qc = useQueryClient();
  const [failed, setFailed] = useState(0);
  const [retrying, setRetrying] = useState(false);

  useEffect(() => {
    const cache = qc.getQueryCache();
    const update = () => setFailed(cache.findAll({ predicate: isFailedActive }).length);
    update();
    return cache.subscribe(update);
  }, [qc]);

  if (failed === 0) return null;
  return (
    <ErrorBanner
      title={title}
      detail={`${failed} request${failed === 1 ? '' : 's'} failed. The device may be offline or restarting.`}
      retrying={retrying}
      onRetry={() => {
        setRetrying(true);
        qc.refetchQueries({ predicate: isFailedActive }).finally(() => setRetrying(false));
      }}
    />
  );
}
