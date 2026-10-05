using System.Collections.Concurrent;

namespace RecompOne.Runtime.Host;

public static class GpuJobs
{
    private sealed class Job
    {
        public Action? Work;
        public readonly ManualResetEventSlim Done = new(false);
        public Exception? Error;
    }
    
    private static readonly ConcurrentQueue<Job> _queue = new();
    private static readonly ConcurrentBag<Job> _spare = [];
    
    private static int _owner = -1;
    
    public static bool Claimed => _owner >= 0;
    
    public static bool IsOwner => _owner == Environment.CurrentManagedThreadId;
    
    public static void Claim()
    {
        _owner = Environment.CurrentManagedThreadId;
    }
    
    public static void Run(Action work)
    {
        if (!Claimed || IsOwner)
        {
            work();
            return;
        }
        
        if (!_spare.TryTake(out var job)) job = new Job();
        
        job.Work = work;
        job.Error = null;
        job.Done.Reset();
        
        _queue.Enqueue(job);
        job.Done.Wait();
        
        var error = job.Error;
        job.Work = null;
        _spare.Add(job);
        
        if (error != null) throw new InvalidOperationException("gpu job faild", error);
    }
    
    public static void Drain()
    {
        while (_queue.TryDequeue(out var job))
        {
            try
            {
                job.Work?.Invoke();
            }
            catch (Exception e)
            {
                job.Error = e;
            }
            
            job.Done.Set();
        }
    }
}
