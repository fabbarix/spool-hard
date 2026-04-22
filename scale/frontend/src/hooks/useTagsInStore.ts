import { useQuery } from '@tanstack/react-query';
import type { TagsInStore } from '../types/api';

export function useTagsInStore() {
  return useQuery<TagsInStore>({
    queryKey: ['tags-in-store'],
    queryFn: () => fetch('/api/tags-in-store').then((r) => r.json()),
  });
}
