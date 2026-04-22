import { Tag } from 'lucide-react';
import { useTagsInStore } from '../../hooks/useTagsInStore';
import { Card } from '@spoolhard/ui/components/Card';
import { Badge } from '@spoolhard/ui/components/Badge';

export function TagsInStore() {
  const { data } = useTagsInStore();
  const tags = data?.tags ? data.tags.split(',').filter(Boolean) : [];

  return (
    <Card
      title="Tags In Store"
      actions={
        <span className="bg-brand-500/20 text-brand-400 text-[10px] font-mono px-2 py-0.5 rounded-full">
          {tags.length}
        </span>
      }
    >
      <div className="flex flex-wrap gap-2">
        {tags.length > 0
          ? tags.map((tag) => <Badge key={tag}>{tag}</Badge>)
          : (
            <div className="flex flex-col items-center justify-center w-full py-4 text-text-muted">
              <Tag size={24} className="mb-2" />
              <span className="text-sm">No tags stored.</span>
            </div>
          )}
      </div>
    </Card>
  );
}
