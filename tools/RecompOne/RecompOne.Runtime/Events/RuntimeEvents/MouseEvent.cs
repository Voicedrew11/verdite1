namespace RecompOne.Runtime.Events;

public enum MouseButton
{
    None = -1,
    Left = 0,
    Right = 1,
    Middle = 2
}

public enum MouseAction
{
    Move,
    Button,
    Wheel
}

/// <summary>
/// the host mouse moved clicked or scrolled
/// </summary>
public sealed class MouseEvent : IEvent
{
    public MouseAction Action;
    public int X, Y;
    public MouseButton Button = MouseButton.None;
    public bool Pressed;
    /// <summary>Notches scrolled, positive away from the user. A float for the
    /// reason <c>InputManager.TakeMouseWheel</c> is one: a trackpad delivers a
    /// fraction of a notch at a time, and an int throws every one of them
    /// away.</summary>
    public float Wheel;
}